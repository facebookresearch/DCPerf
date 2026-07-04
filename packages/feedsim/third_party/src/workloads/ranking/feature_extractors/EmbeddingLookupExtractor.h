// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "FeatureExtractorBase.h"

class EmbeddingLookupExtractor : public FeatureExtractorBase {
 public:
  void initialize(int complexity, int seed) override;

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override;

  std::string name() const override {
    return "EmbeddingLookupExtractor";
  }

 private:
  int num_tables_ = 4;
  int embedding_dim_ = 64;
  int lookups_per_table_ = 20;
  std::vector<std::vector<float>> embedding_tables_;
  std::mt19937_64 rng_;
};
