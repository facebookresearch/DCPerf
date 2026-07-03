// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>
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
  // Optional story content is passed through to CopyContext for
  // data-dependent feature extraction using Silesia corpus data.
  void runFlatExtractors(
      int count,
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      const uint8_t* story_content = nullptr,
      int story_content_length = 0);

 private:
  std::vector<std::unique_ptr<FeatureExtractorBase>> extractors_;

  // Intermediate buffers for chaining extractors
  std::vector<float> dense_buf_a_;
  std::vector<float> dense_buf_b_;
  std::vector<int64_t> sparse_buf_a_;
  std::vector<int64_t> sparse_buf_b_;

  // Flat dispatch state. Concurrency contract: a single
  // FeatureExtractorSuite instance can have runFlatExtractors() called
  // from multiple threads at once (LeafNodeRank dispatches feature
  // extraction onto the multi-threaded GlobalCPUThread pool, and
  // multiple in-flight requests on the same ThreadData share its
  // suite). The fields below are read-only after initializeFlatDispatch
  // — the generated extractor code only does `.find()` on tables and
  // never mutates flat_features_ / flat_copies_ / flat_hash_tables_,
  // so sharing is safe.
  //
  // The previously-member `flat_example_` and shared writes to
  // `flat_struct_data_` were removed: extractor code calls
  // `c->example->idScoreLists[i].emplace_back(...)` and `c->structData[i]
  // += ...`, both racy when shared. runFlatExtractors now uses
  // thread_local buffers for those, with the per-call copy from
  // flat_struct_data_ as the read-only template.
  std::vector<dcperf::feature_extractors::generated::CopyFn> flat_copies_;
  std::atomic<size_t> flat_pos_{0};
  std::unique_ptr<float[]> flat_struct_data_;
  int flat_struct_size_ = 0;
  std::unordered_map<int64_t, float> flat_tables_[4];
  dcperf::mock_hash::MockHashTable flat_hash_tables_[4];
  std::vector<MockFeature> flat_features_;
};
