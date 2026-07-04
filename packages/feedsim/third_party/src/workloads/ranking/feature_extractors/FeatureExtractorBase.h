// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

class FeatureExtractorBase {
 public:
  virtual ~FeatureExtractorBase() = default;

  // Initialize internal data structures based on complexity (1-10) and seed
  virtual void initialize(int complexity, int seed) = 0;

  // Run extraction: read from inputs, write to outputs
  virtual void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) = 0;

  // Human-readable name for logging
  virtual std::string name() const = 0;
};
