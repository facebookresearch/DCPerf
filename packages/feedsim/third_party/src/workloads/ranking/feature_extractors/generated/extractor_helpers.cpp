// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "extractor_helpers.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__x86_64__)
#include <emmintrin.h> // _mm_stream_si32 (non-temporal store)
#endif

#include <folly/FollyMemcpy.h> // folly::__folly_memcpy

namespace dcperf {
namespace feature_extractors {
namespace helpers {

// ======================================================================
// Internal helpers (additional noinline call depth)
// ======================================================================

// Bit-scramble a float entirely in the integer domain (memcpy + integer ALU,
// no FP arithmetic). Always returns a finite, normal float in [0.5, 1.0) so
// downstream isFiniteNonZero()/validity checks behave exactly as before (same
// branch mix). Integer-domain math keeps the instruction mix integer-heavy,
// matching production feed extraction. Op count and call structure are
// preserved so I-cache footprint / BB size are unchanged.
static inline float feBitScramble(float val, uint32_t salt) {
  uint32_t b;
  folly::__folly_memcpy(&b, &val, sizeof(b));
  b ^= salt;
  b *= 2654435761u;
  b ^= b >> 15;
  b *= 2246822519u;
  b ^= b >> 13;
  b = (b & 0x007FFFFFu) | 0x3F000000u; // finite normal float in [0.5, 1.0)
  float o;
  folly::__folly_memcpy(&o, &b, sizeof(o));
  return o;
}

static inline float applyTransform(float val, int transform_type) {
  // Integer-domain transforms (see feBitScramble): no scalar-FP multiplies.
  uint32_t b;
  folly::__folly_memcpy(&b, &val, sizeof(b));
  switch (transform_type % 8) {
    case 0: return val;
    case 1: return feBitScramble(val, 0x7F4A7C15u);
    case 2: return feBitScramble(val, static_cast<uint32_t>(__builtin_popcount(b)));
    case 3: return feBitScramble(val, 0x5BD1E995u);
    case 4: return feBitScramble(val, 0x2545F491u);
    case 5: return feBitScramble(val, 0x9E3779B9u);
    case 6: return feBitScramble(val, b << 3);
    case 7: return feBitScramble(val, (b ^ (b >> 7)) & 0xFFFFu);
    default: return val;
  }
}

__attribute__((noinline))
static float computeBucket(float value, int bucket_type) {
  // Integer-domain bucketing: bit-cast then integer compares/selects instead of
  // float compares + `value * K.0f` scalar-FP multiplies.
  uint32_t b;
  folly::__folly_memcpy(&b, &value, sizeof(b));
  switch (bucket_type % 4) {
    case 0: // Linear-ish bucket
      return feBitScramble(value, 0xA5A5A5A5u);
    case 1: // Log-ish bucket — integer clz, no float
      return feBitScramble(value, static_cast<uint32_t>(__builtin_clz(b | 1u)));
    case 2: // Quantile bucket keyed on the mantissa's high bits
      return feBitScramble(value, (b >> 21) & 0x3u);
    case 3: // Custom-threshold bucket keyed on the exponent field
      return feBitScramble(value, (b >> 23) & 0xFFu);
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
  // Integer-domain rate: combine the numerator/denominator bit patterns with
  // integer ALU instead of the former `numerator * inv_den` scalar-FP multiply.
  uint32_t n_bits, d_bits;
  folly::__folly_memcpy(&n_bits, &numerator, sizeof(n_bits));
  folly::__folly_memcpy(&d_bits, &denominator, sizeof(d_bits));
  uint32_t mixed = n_bits ^ (d_bits * 2654435761u);
  mixed = (mixed & 0x007FFFFFu) | 0x3F000000u; // finite float in [0.5, 1.0)
  float rate;
  folly::__folly_memcpy(&rate, &mixed, sizeof(rate));
  rate = applyTransform(rate, bucket_type);
  return computeBucket(rate, bucket_type);
}

float computeEngagementStat(
    const float* stats, int num_stats,
    int window_type, int stat_idx) {
  int window_size = 1 + (window_type % 4);  // 1-4
  int start = stat_idx % num_stats;
  // Integer-domain accumulate over the stat bit patterns (FNV-style): keeps the
  // windowed memory-read loop but drops the scalar-FP `stats[i] * w` mul-adds.
  uint32_t acc = 0x811C9DC5u;
  for (int i = 0; i < window_size && (start + i) < num_stats; ++i) {
    uint32_t s;
    folly::__folly_memcpy(&s, &stats[(start + i) % num_stats], sizeof(s));
    acc = (acc ^ (s + static_cast<uint32_t>(i))) * 16777619u;
  }
  acc = (acc & 0x007FFFFFu) | 0x3F000000u; // finite float in [0.5, 1.0)
  float out;
  folly::__folly_memcpy(&out, &acc, sizeof(out));
  return applyTransform(out, window_type);
}

float aggregateRates(
    const float* rates, int num_rates,
    int agg_type, float scale) {
  if (num_rates <= 0) return 0.0f;
  // Integer-domain aggregation over the rate bit patterns: preserves the
  // per-agg-type loop shape / branch mix but removes scalar-FP add/mul chains.
  uint32_t acc;
  folly::__folly_memcpy(&acc, &scale, sizeof(acc));
  switch (agg_type % 4) {
    case 0: // Sum-like
      for (int i = 0; i < num_rates; ++i) {
        uint32_t r;
        folly::__folly_memcpy(&r, &rates[i], sizeof(r));
        acc += r;
      }
      break;
    case 1: { // Max-like
      folly::__folly_memcpy(&acc, &rates[0], sizeof(acc));
      for (int i = 1; i < num_rates; ++i) {
        uint32_t r;
        folly::__folly_memcpy(&r, &rates[i], sizeof(r));
        if (r > acc) acc = r;
      }
      break;
    }
    case 2: // Weighted-mean-like
      for (int i = 0; i < num_rates; ++i) {
        uint32_t r;
        folly::__folly_memcpy(&r, &rates[i], sizeof(r));
        acc = (acc ^ (r >> (i & 7))) * 2654435761u;
      }
      break;
    case 3: // Geometric-mean-like
      for (int i = 0; i < num_rates; ++i) {
        uint32_t r;
        folly::__folly_memcpy(&r, &rates[i], sizeof(r));
        acc = (acc + r) * 2246822519u;
      }
      acc = static_cast<uint32_t>(__builtin_clz(acc | 1u));
      break;
  }
  acc = (acc & 0x007FFFFFu) | 0x3F000000u; // finite float in [0.5, 1.0)
  float result;
  folly::__folly_memcpy(&result, &acc, sizeof(result));
  return computeBucket(result, agg_type);
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

// ======================================================================
// Memory-streaming stride sweep (backend/DRAM-pressure lever)
// ======================================================================

namespace {

struct SweepConfig {
  std::vector<float> buf;  // process-wide, read-only after init
  size_t size = 0;         // element count
  int n = 0;               // reads per extractor call (FEEDSIM_SWEEP_N)
  size_t stride = 16;      // element stride (FEEDSIM_SWEEP_STRIDE); 16 = 64B line
  std::vector<float> wbuf; // separate write target (allocated only when wn > 0)
  size_t wsize = 0;        // write buffer element count
  int wn = 0;              // writes per extractor call (FEEDSIM_SWEEP_WN)
  size_t wstride = 16;     // write stride (FEEDSIM_SWEEP_WSTRIDE)
};

SweepConfig g_sweep;
std::once_flag g_sweep_once;

// Non-temporal (streaming) float store: writes straight to memory, bypassing
// the cache and its read-for-ownership, so the traffic lands as pure DRAM
// writes rather than the read+write a normal store incurs. Falls back to a
// plain store where a streaming store isn't available (e.g. aarch64 + GCC).
inline void ntStoreFloat(float* dst, float value) {
#if defined(__x86_64__)
  int bits;
  folly::__folly_memcpy(&bits, &value, sizeof(bits));
  _mm_stream_si32(reinterpret_cast<int*>(dst), bits);
#elif defined(__aarch64__) && defined(__clang__)
  __builtin_nontemporal_store(value, dst);
#else
  *dst = value;
#endif
}

int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return fallback;
  }
  int parsed = std::atoi(v);
  return parsed;
}

void initSweep() {
  // Defaults: 16 strided reads/call over a 64 MB DRAM-resident buffer at a
  // 64 B (1 cache line) stride. This adds DRAM-bandwidth / LLC / L1-D pressure
  // to move the memory hierarchy toward prod. Override any knob via the
  // FEEDSIM_SWEEP_* env vars; set FEEDSIM_SWEEP_N=0 to disable entirely.
  int mb = envInt("FEEDSIM_SWEEP_MB", 64);
  if (mb < 1) {
    mb = 1;
  }
  g_sweep.n = envInt("FEEDSIM_SWEEP_N", 16);
  if (g_sweep.n < 0) {
    g_sweep.n = 0;
  }
  int stride = envInt("FEEDSIM_SWEEP_STRIDE", 16);
  g_sweep.stride = stride < 1 ? 1 : static_cast<size_t>(stride);
  g_sweep.size = static_cast<size_t>(mb) * 1024 * 1024 / sizeof(float);
  g_sweep.buf.resize(g_sweep.size);
  // Fill with pseudo-random data so the compiler can't fold the buffer away.
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  for (size_t i = 0; i < g_sweep.size; ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    g_sweep.buf[i] = static_cast<float>((s >> 40) & 0xFFFF) * 1e-3f;
  }

  // Companion write sweep: the read sweep above only adds read traffic, which
  // leaves feedsim's read:write ratio above prod's. FEEDSIM_SWEEP_WN streaming
  // writes/call over a separate DRAM-resident buffer raise the write share to
  // move the ratio toward prod. Default 0 disables the sweep (no-op, skips
  // buffer allocation); set FEEDSIM_SWEEP_WN>0 to enable. Write stride/size
  // default to the read knobs.
  g_sweep.wn = envInt("FEEDSIM_SWEEP_WN", 0);
  if (g_sweep.wn < 0) {
    g_sweep.wn = 0;
  }
  int wstride = envInt("FEEDSIM_SWEEP_WSTRIDE", stride);
  g_sweep.wstride = wstride < 1 ? 1 : static_cast<size_t>(wstride);
  if (g_sweep.wn > 0) {
    int wmb = envInt("FEEDSIM_SWEEP_WMB", mb);
    if (wmb < 1) {
      wmb = 1;
    }
    g_sweep.wsize = static_cast<size_t>(wmb) * 1024 * 1024 / sizeof(float);
    g_sweep.wbuf.resize(g_sweep.wsize);
  }
}

} // namespace

int sweepReadsPerCall() {
  std::call_once(g_sweep_once, initSweep);
  return g_sweep.n;
}

float runStrideSweep(uint64_t seed) {
  const SweepConfig& c = g_sweep;
  if (c.n == 0 || c.size == 0) {
    return 0.0f;
  }
  // Rotate the start offset per call so successive calls cover the whole
  // buffer rather than re-touching one region.
  size_t off = (seed * 2654435761ULL) % c.size;
  float acc = 0.0f;
  for (int i = 0; i < c.n; ++i) {
    acc += c.buf[off];
    off += c.stride;
    if (off >= c.size) {
      off -= c.size;
    }
  }
  return acc;
}

int sweepWritesPerCall() {
  std::call_once(g_sweep_once, initSweep);
  return g_sweep.wn;
}

float runStrideSweepWrite(uint64_t seed) {
  SweepConfig& c = g_sweep;
  if (c.wn == 0 || c.wsize == 0) {
    return 0.0f;
  }
  // Rotate the start offset per call so successive calls stream over the whole
  // buffer instead of hammering one region.
  size_t off = (seed * 2654435761ULL) % c.wsize;
  for (int i = 0; i < c.wn; ++i) {
    ntStoreFloat(&c.wbuf[off], static_cast<float>(seed) + static_cast<float>(i));
    off += c.wstride;
    if (off >= c.wsize) {
      off -= c.wsize;
    }
  }
  // Read one element from a region we didn't just write so the caller can fold
  // it into live state; this keeps the streaming stores from being elided
  // without stalling on the store buffer we just filled.
  return c.wbuf[(off + c.wsize / 2) % c.wsize];
}

} // namespace helpers
} // namespace feature_extractors
} // namespace dcperf
