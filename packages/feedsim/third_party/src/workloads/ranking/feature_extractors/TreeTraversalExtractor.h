// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "FeatureExtractorBase.h"
#include "FeatureTypes.h"

class TreeTraversalExtractor : public FeatureExtractorBase {
 public:
  void initialize(int complexity, int seed) override;

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override;

  std::string name() const override {
    return "TreeTraversalExtractor";
  }

 private:
  int maps_per_call_ = 2;
  std::vector<std::map<int64_t, float>> maps_;
  std::vector<MockFeature> features_;
  MockFeatureExample example_;
};
