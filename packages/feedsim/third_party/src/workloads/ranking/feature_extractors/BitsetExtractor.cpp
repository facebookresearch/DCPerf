// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "BitsetExtractor.h"

#include <cmath>

void BitsetExtractor::initialize(int complexity, int seed) {
  rng_.seed(seed);

  bitset_size_ = 500 + complexity * 300;
  checks_per_call_ = 20 + complexity * 20;
  conversions_per_call_ = 10 + complexity * 10;

  // Initialize bitset with ~70% of bits set
  feature_flags_.resize(bitset_size_);
  for (int i = 0; i < bitset_size_; ++i) {
    feature_flags_[i] = (rng_() % 10) < 7;
  }

  // Set up features
  features_.resize(checks_per_call_);
  for (int i = 0; i < checks_per_call_; ++i) {
    features_[i].raw_feature_index = i % 25;
    features_[i].is_indexed = true;
    features_[i].use_all_indexes = true;
  }
}

void BitsetExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  int total_sparse = std::min(25, checks_per_call_);
  example_.resize(0, total_sparse, 0);

  output_dense.clear();
  output_sparse.clear();

  std::uniform_int_distribution<int> bit_dist(0, bitset_size_ - 1);
  std::uniform_real_distribution<double> raw_dist(-1e6, 1e6);

  int feat_idx = 0;

  // Phase 1: Random bitset reads + conditional feature yield
  for (int i = 0; i < checks_per_call_; ++i) {
    int bit_index = bit_dist(rng_);
    bool is_requested = feature_flags_[bit_index];

    if (is_requested && feat_idx < static_cast<int>(features_.size())) {
      // Generate raw double and apply capped type conversion
      double raw_value = raw_dist(rng_);
      float val = cappedConvert<float>(raw_value);
      yieldIndexedFeature(
          example_, features_[feat_idx], bit_index, val);
      output_dense.push_back(val);
      output_sparse.push_back(bit_index);
      ++feat_idx;
    }
  }

  // Phase 2: Additional type conversions (cappedTypeConversion pattern)
  for (int i = 0; i < conversions_per_call_; ++i) {
    double raw = raw_dist(rng_);
    float converted = cappedConvert<float>(raw);
    // Use the result to prevent optimization
    if (isFiniteAndNonZero(converted)) {
      output_dense.push_back(converted);
    }
  }
}
