// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ContainerExtractor.h"

void ContainerExtractor::initialize(int complexity, int seed) {
  rng_.seed(seed);

  // Scale with complexity
  num_small_vectors_ = 10 + complexity * 5;
  items_per_small_ = 4 + complexity * 2;
  num_vectors_ = 5 + complexity * 2;
  items_per_vector_ = 20 + complexity * 10;

  int total_features = num_small_vectors_ + num_vectors_;
  features_.resize(total_features);
  for (int i = 0; i < total_features; ++i) {
    features_[i].raw_feature_index = i % 40;
    features_[i].is_indexed = true;
    features_[i].use_all_indexes = true;
  }
}

void ContainerExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  int total_features = num_small_vectors_ + num_vectors_;
  example_.resize(0, std::min(40, total_features), 0);

  std::uniform_real_distribution<float> score_dist(0.01f, 5.0f);
  std::uniform_int_distribution<int64_t> id_dist(1, 1000000);

  int feat_idx = 0;

  // Phase 1: small_vector-like appends (inline then heap)
  // Using std::vector with small initial sizes to mimic small_vector behavior
  for (int v = 0; v < num_small_vectors_; ++v) {
    std::vector<IdScorePair> sv;
    // Deliberately don't reserve — reallocation is part of the footprint
    for (int i = 0; i < items_per_small_; ++i) {
      int64_t id = id_dist(rng_);
      float score = score_dist(rng_);
      sv.emplace_back(IdScorePair{id, score});
    }
    // Yield collected features through the pipeline
    if (feat_idx < static_cast<int>(features_.size())) {
      for (const auto& pair : sv) {
        yieldIndexedFeature(
            example_, features_[feat_idx], pair.id, pair.score);
      }
    }
    ++feat_idx;
  }

  // Phase 2: std::vector appends with natural growth
  for (int v = 0; v < num_vectors_; ++v) {
    std::vector<float> collected;
    // Deliberately don't reserve — let it grow and reallocate
    for (int i = 0; i < items_per_vector_; ++i) {
      collected.emplace_back(score_dist(rng_));
    }
    // Use accumulated values
    if (feat_idx < static_cast<int>(features_.size())) {
      for (size_t i = 0; i < collected.size(); ++i) {
        yieldIndexedFeature(
            example_,
            features_[feat_idx],
            static_cast<int64_t>(i),
            collected[i]);
      }
    }
    ++feat_idx;
  }

  // Copy results to output
  output_dense.clear();
  output_sparse.clear();
  for (const auto& list : example_.idScoreLists) {
    for (const auto& pair : list) {
      output_dense.push_back(pair.score);
      output_sparse.push_back(pair.id);
    }
  }
}
