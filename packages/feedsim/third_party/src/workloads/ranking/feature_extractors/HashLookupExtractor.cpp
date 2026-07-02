// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "HashLookupExtractor.h"

void HashLookupExtractor::initialize(int complexity, int seed) {
  rng_.seed(seed);

  // Scale table size with complexity to control cache pressure
  // complexity 1: 1K entries (~16KB, fits L1)
  // complexity 5: 50K entries (~800KB, spills L2)
  // complexity 10: 500K entries (~8MB, spills L3 on some CPUs)
  int table_size = 1000 * complexity * complexity;
  num_tables_ = std::max(2, complexity);
  lookups_per_table_ = 10 + complexity * 4;

  tables_.resize(num_tables_);
  std::uniform_real_distribution<float> val_dist(0.01f, 100.0f);

  for (auto& table : tables_) {
    table.reserve(table_size);
    for (int i = 0; i < table_size; ++i) {
      int64_t key = static_cast<int64_t>(i) * 7 + seed;
      table[key] = val_dist(rng_);
    }
  }

  // Set up features for yield pipeline
  int total_features = num_tables_ * lookups_per_table_;
  features_.resize(total_features);
  for (int i = 0; i < total_features; ++i) {
    features_[i].raw_feature_index = i % 50;
    features_[i].is_indexed = true;
    features_[i].use_all_indexes = true;
  }
}

void HashLookupExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  int total_features = num_tables_ * lookups_per_table_;
  example_.resize(
      static_cast<int>(input_dense.size()),
      std::min(50, total_features),
      0);

  // Generate query keys from input sparse features
  std::vector<int64_t> query_keys;
  if (!input_sparse.empty()) {
    query_keys = input_sparse;
  } else {
    query_keys.resize(lookups_per_table_);
    std::uniform_int_distribution<int64_t> key_dist(0, 1000000);
    for (auto& k : query_keys) {
      k = key_dist(rng_);
    }
  }

  int feat_idx = 0;
  for (const auto& table : tables_) {
    for (int i = 0; i < lookups_per_table_; ++i) {
      int64_t key = query_keys[i % query_keys.size()];
      auto it = table.find(key);
      float val = (it != table.end()) ? it->second : 0.0f;
      if (feat_idx < static_cast<int>(features_.size())) {
        yieldIndexedFeature(example_, features_[feat_idx], key, val);
      }
      ++feat_idx;
    }
  }

  // Copy results to output vectors
  output_dense = example_.denseValues;
  output_sparse.clear();
  for (const auto& list : example_.idScoreLists) {
    for (const auto& pair : list) {
      output_sparse.push_back(pair.id);
    }
  }
}
