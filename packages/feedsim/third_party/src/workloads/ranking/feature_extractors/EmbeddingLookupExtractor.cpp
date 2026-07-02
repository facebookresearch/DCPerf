// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "EmbeddingLookupExtractor.h"

#include <algorithm>

void EmbeddingLookupExtractor::initialize(int complexity, int seed) {
  rng_.seed(seed);

  // Scale embedding table size with complexity to control cache pressure
  // complexity 1: 1K rows x 32 dim (~128KB)
  // complexity 5: 25K rows x 64 dim (~6.4MB, spills L2)
  // complexity 10: 100K rows x 128 dim (~51MB, spills L3)
  int num_rows = 1000 * complexity * complexity;
  embedding_dim_ = 32 + complexity * 10;
  num_tables_ = std::max(2, complexity / 2);
  lookups_per_table_ = 10 + complexity * 3;

  embedding_tables_.resize(num_tables_);
  std::uniform_real_distribution<float> val_dist(-1.0f, 1.0f);

  for (auto& table : embedding_tables_) {
    table.resize(static_cast<size_t>(num_rows) * embedding_dim_);
    for (auto& v : table) {
      v = val_dist(rng_);
    }
  }
}

void EmbeddingLookupExtractor::extract(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::vector<float>& output_dense,
    std::vector<int64_t>& output_sparse) {
  // Output: accumulated embedding vectors
  output_dense.assign(embedding_dim_, 0.0f);
  output_sparse.clear();

  for (int t = 0; t < num_tables_; ++t) {
    const auto& table = embedding_tables_[t];
    int num_rows =
        static_cast<int>(table.size()) / embedding_dim_;

    for (int l = 0; l < lookups_per_table_; ++l) {
      // Determine row to look up: use input sparse IDs or generate random
      int64_t row_idx;
      int sparse_idx = t * lookups_per_table_ + l;
      if (sparse_idx < static_cast<int>(input_sparse.size())) {
        row_idx = std::abs(input_sparse[sparse_idx]) % num_rows;
      } else {
        std::uniform_int_distribution<int64_t> row_dist(0, num_rows - 1);
        row_idx = row_dist(rng_);
      }

      // Random access into the embedding table (cache-unfriendly)
      const float* row_ptr =
          table.data() + row_idx * embedding_dim_;

      // Accumulate into output (sum pooling)
      for (int d = 0; d < embedding_dim_; ++d) {
        output_dense[d] += row_ptr[d];
      }

      output_sparse.push_back(row_idx);
    }
  }
}
