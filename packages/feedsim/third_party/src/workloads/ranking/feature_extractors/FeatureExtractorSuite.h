// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "FeatureExtractorBase.h"
#include "FeatureTypes.h"
#include "generated/dispatch.h"
#include "generated/mock_hash_table.h"
#include "generated/registry.h"

class FeatureExtractorSuite {
 public:
  FeatureExtractorSuite() = default;

  // Add an extractor to the suite
  void addExtractor(std::unique_ptr<FeatureExtractorBase> extractor);

  // Initialize all extractors with the given complexity and seed
  void initializeAll(int complexity, int seed);

  // Run all extractors in sequence, chaining outputs to inputs
  void runAll(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse);

  // Run a random subset of extractors (simulating per-story extraction)
  // Picks 'count' extractors randomly from the pool and runs them
  void runRandomSubset(
      int count,
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::default_random_engine& rng);

  // Get number of registered extractors
  size_t size() const;

  // Initialize flat copy dispatch: collects all copy functions, shuffles them
  void initializeFlatDispatch(int seed);

  // Run N sequential copy functions from the shuffled flat vector
  void runFlatExtractors(
      int count,
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse);

 private:
  std::vector<std::unique_ptr<FeatureExtractorBase>> extractors_;

  // Intermediate buffers for chaining extractors
  std::vector<float> dense_buf_a_;
  std::vector<float> dense_buf_b_;
  std::vector<int64_t> sparse_buf_a_;
  std::vector<int64_t> sparse_buf_b_;

  // Flat dispatch state
  std::vector<dcperf::feature_extractors::generated::CopyFn> flat_copies_;
  size_t flat_pos_ = 0;
  std::unique_ptr<float[]> flat_struct_data_;
  int flat_struct_size_ = 0;
  std::unordered_map<int64_t, float> flat_tables_[4];
  dcperf::mock_hash::MockHashTable flat_hash_tables_[4];
  MockFeatureExample flat_example_;
  std::vector<MockFeature> flat_features_;
};
