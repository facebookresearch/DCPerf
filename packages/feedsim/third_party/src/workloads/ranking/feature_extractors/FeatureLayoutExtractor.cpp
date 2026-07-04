// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FeatureLayoutExtractor.h"

void FeatureLayoutExtractor::initialize(int complexity, int seed) {
  rng_.seed(seed);

  // Scale with complexity
  num_dense_ = 200 + complexity * 100;
  num_remapped_ = 50 + complexity * 40;
  int num_sparse = 10 + complexity * 10;
  int sparse_remap_count = 5 + complexity * 5;

  // Build dense remap table with randomized (to, from) pairs
  std::uniform_int_distribution<int> dense_dist(0, num_dense_ - 1);
  dense_remap_.resize(num_remapped_);
  for (auto& [to, from] : dense_remap_) {
    to = dense_dist(rng_);
    from = dense_dist(rng_);
  }

  // Build sparse remap table
  std::uniform_int_distribution<int> sparse_dist(0, num_sparse - 1);
  sparse_remap_.resize(sparse_remap_count);
  for (auto& [to, from] : sparse_remap_) {
    to = sparse_dist(rng_);
    from = sparse_dist(rng_);
  }

  // Pre-fill cache example with random values
  cache_example_.denseValues.resize(num_dense_);
  std::uniform_real_distribution<float> val_dist(0.01f, 10.0f);
  for (auto& v : cache_example_.denseValues) {
    v = val_dist(rng_);
  }

  cache_example_.idScoreLists.resize(num_sparse);
  std::uniform_int_distribution<int64_t> id_dist(0, 1000000);
  for (auto& list : cache_example_.idScoreLists) {
    int list_size = 1 + (rng_() % 5);
    for (int i = 0; i < list_size; ++i) {
      list.emplace_back(IdScorePair{id_dist(rng_), val_dist(rng_)});
    }
  }
}

void FeatureLayoutExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  // Prepare output arrays sized to match the cache
  output_dense.assign(num_dense_, 0.0f);

  // Dense copy with indirect indexing (strided scatter pattern)
  float* local = output_dense.data();
  const float* cache = cache_example_.denseValues.data();
  for (const auto& [to, from] : dense_remap_) {
    local[to] = cache[from];
  }

  // Also copy from input if available
  int copy_count =
      std::min(static_cast<int>(input_dense.size()), num_dense_);
  for (int i = 0; i < copy_count; ++i) {
    int dst = dense_remap_[i % dense_remap_.size()].first;
    output_dense[dst] += input_dense[i];
  }

  // Sparse copy via vector assignment
  output_sparse.clear();
  for (const auto& [to, from] : sparse_remap_) {
    if (from < static_cast<int>(cache_example_.idScoreLists.size())) {
      for (const auto& pair : cache_example_.idScoreLists[from]) {
        output_sparse.push_back(pair.id);
      }
    }
  }
}
