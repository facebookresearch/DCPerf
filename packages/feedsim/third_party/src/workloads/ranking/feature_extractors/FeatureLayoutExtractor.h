// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "FeatureExtractorBase.h"
#include "FeatureTypes.h"

class FeatureLayoutExtractor : public FeatureExtractorBase {
 public:
  void initialize(int complexity, int seed) override;

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override;

  std::string name() const override {
    return "FeatureLayoutExtractor";
  }

 private:
  int num_dense_ = 500;
  int num_remapped_ = 200;
  std::vector<std::pair<int, int>> dense_remap_;
  std::vector<std::pair<int, int>> sparse_remap_;
  MockFeatureExample cache_example_;
  std::mt19937 rng_;
};
