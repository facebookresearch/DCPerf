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

class ContainerExtractor : public FeatureExtractorBase {
 public:
  void initialize(int complexity, int seed) override;

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override;

  std::string name() const override {
    return "ContainerExtractor";
  }

 private:
  int num_small_vectors_ = 30;
  int items_per_small_ = 12;
  int num_vectors_ = 10;
  int items_per_vector_ = 50;
  std::vector<MockFeature> features_;
  MockFeatureExample example_;
  std::mt19937 rng_;
};
