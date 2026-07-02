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
#include "FeatureTypes.h"

class BitsetExtractor : public FeatureExtractorBase {
 public:
  void initialize(int complexity, int seed) override;

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override;

  std::string name() const override {
    return "BitsetExtractor";
  }

 private:
  int bitset_size_ = 2000;
  int checks_per_call_ = 100;
  int conversions_per_call_ = 50;
  std::vector<bool> feature_flags_;
  std::vector<MockFeature> features_;
  MockFeatureExample example_;
  std::mt19937 rng_;
};
