// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "TreeTraversalExtractor.h"

void TreeTraversalExtractor::initialize(int complexity, int seed) {
  std::mt19937_64 rng(seed);

  // Scale map size and count with complexity
  int map_size = 100 + complexity * 100;
  maps_per_call_ = std::max(1, complexity / 3);

  maps_.resize(maps_per_call_);
  std::uniform_int_distribution<int64_t> key_dist(0, 10000000);
  std::uniform_real_distribution<float> val_dist(0.01f, 50.0f);

  for (auto& map : maps_) {
    for (int i = 0; i < map_size; ++i) {
      map[key_dist(rng)] = val_dist(rng);
    }
  }

  // Set up features
  int total_features = maps_per_call_ * map_size;
  features_.resize(total_features);
  for (int i = 0; i < total_features; ++i) {
    features_[i].raw_feature_index = i % 30;
    features_[i].is_indexed = true;
    features_[i].use_all_indexes = true;
  }
}

void TreeTraversalExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  int total_sparse = std::min(30, static_cast<int>(features_.size()));
  example_.resize(0, total_sparse, 0);

  output_dense.clear();
  output_sparse.clear();

  int feat_idx = 0;
  for (const auto& map : maps_) {
    // Full ordered traversal — each iterator increment follows
    // parent/child pointers through the red-black tree (poor spatial locality)
    for (const auto& [key, value] : map) {
      if (feat_idx < static_cast<int>(features_.size())) {
        yieldIndexedFeature(example_, features_[feat_idx], key, value);
      }
      output_dense.push_back(value);
      output_sparse.push_back(key);
      ++feat_idx;
    }
  }
}
