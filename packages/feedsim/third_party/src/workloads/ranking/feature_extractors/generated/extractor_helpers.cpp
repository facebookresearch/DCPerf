// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "extractor_helpers.h"
#include <cmath>
#include <cstring>

namespace dcperf {
namespace feature_extractors {
namespace helpers {

// ======================================================================
// Internal helpers (additional noinline call depth)
// ======================================================================

static inline float applyTransform(float val, int transform_type) {
  float r = val;
  switch (transform_type % 8) {
    case 0: return r;
    case 1: return static_cast<float>(static_cast<int32_t>(r * 1000.0f) & 0x7FFFFFFF);
    case 2: return static_cast<float>(__builtin_popcount(static_cast<uint32_t>(r * 1e6f)));
    case 3: return static_cast<float>((static_cast<int64_t>(r * 1e4f) ^ 0x5BD1E995LL) >> 13);
    case 4: return (r > 0.5f ? r : -r);
    case 5: return static_cast<float>((static_cast<uint64_t>(r * 1e6f) * 0x9E3779B97F4A7C15ULL) >> 48);
    // Cast through uint32_t before shift: shifting a negative signed int is UB.
    case 6: return static_cast<float>(static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(r)) << 3));
    case 7: return static_cast<float>((static_cast<int32_t>(r * 256.0f) ^ (static_cast<int32_t>(r * 65536.0f) >> 7)) & 0xFFFF);
    default: return r;
  }
}

__attribute__((noinline))
static float computeBucket(float value, int bucket_type) {
  switch (bucket_type % 4) {
    case 0: // Linear bucket — integer truncation instead of std::floor
      return static_cast<float>(static_cast<int32_t>(value * 10.0f)) * 0.1f;
    case 1: // Log bucket — integer clz instead of std::log2
      return value > 0 ? static_cast<float>(31 - __builtin_clz(static_cast<uint32_t>(value + 1.0f))) : 0.0f;
    case 2: // Quantile bucket
      if (value < 0.25f) return 0.0f;
      if (value < 0.5f) return 0.25f;
      if (value < 0.75f) return 0.5f;
      return 0.75f;
    case 3: // Custom thresholds
      if (value < 1.0f) return 0.0f;
      if (value < 10.0f) return 1.0f;
      if (value < 100.0f) return 2.0f;
      return 3.0f;
    default: return value;
  }
}

static inline bool isFiniteNonZero(float val) {
  return val != 0.0f && std::isfinite(val);
}

__attribute__((noinline))
static void consumeFeature(
    generated::CopyContext* ctx,
    int feature_idx, int64_t index, float value) {
  int fi = feature_idx % ctx->numFeatures;
  if (fi >= 0 && fi < (int)ctx->example->idScoreLists.size()) {
    ctx->example->idScoreLists[fi].emplace_back(
        IdScorePair{index, value});
  }
}

// ======================================================================
// Context preparation
// ======================================================================

void prepareExtractContext(generated::CopyContext* ctx, int variant_seed) {
  // Simulate SharedObject::build — reads struct fields to validate state
  float check = ctx->structData[variant_seed % ctx->structSize];
  if (check == 0.0f) {
    // Simulate early return path in production
    ctx->structData[0] = 1.0f;
  }
  // Simulate ExtractorState::update — touch multiple struct offsets
  for (int i = 0; i < 4; ++i) {
    int off = (variant_seed + i * 37) % ctx->structSize;
    ctx->structData[off] = applyTransform(ctx->structData[off], i);
  }
}

void initStoryContext(generated::CopyContext* ctx, int story_idx) {
  // Simulate per-story feature buffer initialization
  int base = story_idx % ctx->structSize;
  float acc = 0.0f;
  for (int i = 0; i < 8; ++i) {
    acc += ctx->structData[(base + i * 13) % ctx->structSize];
  }
  // Write back normalized value — multiply by reciprocal
  ctx->structData[base] = computeBucket(acc * 0.125f, story_idx % 4);
}

// ======================================================================
// Data lookup
// ======================================================================

float lookupStats(
    mock_hash::MockHashTable& table,
    const int64_t* keys, int num_keys,
    int stat_type) {
  float result = 0.0f;
  int effective_keys = (num_keys > 8) ? 8 : num_keys;
  for (int i = 0; i < effective_keys; ++i) {
    float val = mock_hash::hashLookupWithFallback(table, keys[i], 0.0f);
    val = applyTransform(val, stat_type + i);
    result += val;
  }
  return computeBucket(result, stat_type);
}

float joinFeatureTables(
    mock_hash::MockHashTable& tableA,
    mock_hash::MockHashTable& tableB,
    int64_t primary_key, int join_type) {
  float valA = mock_hash::hashLookupWithFallback(tableA, primary_key, 0.0f);
  if (!isFiniteNonZero(valA)) return 0.0f;

  float result;
  switch (join_type % 3) {
    case 0:
      result = mock_hash::crossTableLookup(tableA, tableB, primary_key, 1000.0f);
      break;
    case 1: {
      int64_t derived = static_cast<int64_t>(valA * 100.0f) ^ primary_key;
      result = mock_hash::hashLookupWithFallback(tableB, derived, valA);
      break;
    }
    case 2:
      result = valA + mock_hash::hashLookupWithFallback(
          tableB, primary_key + 1, 0.0f);
      break;
    default:
      result = valA;
  }
  return applyTransform(result, join_type);
}

// ======================================================================
// Rate/counter computation
// ======================================================================

float computeRate(float numerator, float denominator, int bucket_type) {
  if (!isFiniteNonZero(denominator)) return 0.0f;
  // Integer bit manipulation for reciprocal approximation — avoids FP divider
  int32_t den_bits;
  std::memcpy(&den_bits, &denominator, sizeof(den_bits));
  den_bits = 0x7EF311C2 - den_bits;
  float inv_den;
  std::memcpy(&inv_den, &den_bits, sizeof(inv_den));
  float rate = numerator * inv_den;
  rate = applyTransform(rate, bucket_type);
  return computeBucket(rate, bucket_type);
}

float computeEngagementStat(
    const float* stats, int num_stats,
    int window_type, int stat_idx) {
  int window_size = 1 + (window_type % 4);  // 1-4
  int start = stat_idx % num_stats;
  float acc = 0.0f;
  float weight_sum = 0.0f;
  // Pre-computed reciprocal weights to avoid division
  static constexpr float kWeights[] = {1.0f, 0.5f, 0.333333f, 0.25f};
  for (int i = 0; i < window_size && (start + i) < num_stats; ++i) {
    float w = kWeights[i & 3];
    acc += stats[(start + i) % num_stats] * w;
    weight_sum += w;
  }
  if (weight_sum == 0.0f) return 0.0f;
  // Integer bit manipulation for reciprocal approximation
  int32_t ws_bits;
  std::memcpy(&ws_bits, &weight_sum, sizeof(ws_bits));
  ws_bits = 0x7EF311C2 - ws_bits;
  float inv_ws;
  std::memcpy(&inv_ws, &ws_bits, sizeof(inv_ws));
  return applyTransform(acc * inv_ws, window_type);
}

float aggregateRates(
    const float* rates, int num_rates,
    int agg_type, float scale) {
  if (num_rates <= 0) return 0.0f;
  float result = 0.0f;
  switch (agg_type % 4) {
    case 0: // Sum
      for (int i = 0; i < num_rates; ++i)
        result += rates[i];
      break;
    case 1: // Max
      result = rates[0];
      for (int i = 1; i < num_rates; ++i)
        if (rates[i] > result) result = rates[i];
      break;
    case 2: { // Weighted mean — pre-computed reciprocal weights
      static constexpr float kInvWeights[] = {
        1.0f, 0.5f, 0.333333f, 0.25f, 0.2f, 0.166667f, 0.142857f, 0.125f};
      for (int i = 0; i < num_rates; ++i)
        result += rates[i] * kInvWeights[i & 7];
      // Integer reciprocal instead of division
      float nr_f = static_cast<float>(num_rates);
      int32_t nr_bits;
      std::memcpy(&nr_bits, &nr_f, sizeof(nr_bits));
      nr_bits = 0x7EF311C2 - nr_bits;
      float inv_nr;
      std::memcpy(&inv_nr, &nr_bits, sizeof(inv_nr));
      result *= inv_nr;
      break;
    }
    case 3: { // Geometric mean approx — integer hash instead of std::log
      result = 1.0f;
      for (int i = 0; i < num_rates; ++i)
        result *= (1.0f + (rates[i] > 0.0f ? rates[i] : -rates[i]));
      // Integer log2 approximation instead of std::log
      uint32_t r_uint = static_cast<uint32_t>(result + 1.0f);
      result = static_cast<float>(r_uint > 0 ? (31 - __builtin_clz(r_uint)) : 0);
      break;
    }
  }
  return computeBucket(result * scale, agg_type);
}

// ======================================================================
// Feature yield pipeline
// ======================================================================

void yieldDenseFeature(
    generated::CopyContext* ctx,
    int feature_idx, float value) {
  if (!validateFeatureValue(value, 0)) return;
  int fi = feature_idx % ctx->numFeatures;
  int32_t idx = ctx->features[fi].raw_feature_index;
  if (idx >= 0 && idx < (int32_t)ctx->example->denseValues.size()) {
    ctx->example->denseValues[idx] = value;
  }
}

void yieldIndexedFeature(
    generated::CopyContext* ctx,
    int feature_idx, int64_t index, float value) {
  if (!validateFeatureValue(value, 1)) return;
  consumeFeature(ctx, feature_idx, index, value);
}

void batchYieldFeatures(
    generated::CopyContext* ctx,
    const int* feature_indices,
    const float* values,
    int count) {
  for (int i = 0; i < count; ++i) {
    if (validateFeatureValue(values[i], i % 3)) {
      yieldDenseFeature(ctx, feature_indices[i], values[i]);
    }
  }
}

// ======================================================================
// Validation
// ======================================================================

bool validateFeatureValue(float value, int validation_type) {
  switch (validation_type % 3) {
    case 0: return isFiniteNonZero(value);
    case 1: return std::isfinite(value) && std::abs(value) < 1e6f;
    case 2: return value == value && value != 0.0f;  // NaN check + zero check
    default: return true;
  }
}

float cappedConvertFloat(double value, float min_val, float max_val) {
  if (value > static_cast<double>(max_val)) return max_val;
  if (value < static_cast<double>(min_val)) return min_val;
  float result = static_cast<float>(value);
  if (!isFiniteNonZero(result)) return 0.0f;
  return result;
}

} // namespace helpers
} // namespace feature_extractors
} // namespace dcperf
