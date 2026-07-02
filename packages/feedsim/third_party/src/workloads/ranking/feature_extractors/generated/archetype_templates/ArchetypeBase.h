// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "../../FeatureExtractorBase.h"
#include "../../FeatureTypes.h"
#include "ArchetypeUtils.h"
#include <algorithm>
#include <vector>

namespace dcperf {
namespace feature_extractors {
namespace generated {

// Adapter between FeatureExtractorBase (4-vector interface) and the archetype
// internal interface (MockFeatureExample + MockFeature). Archetypes override
// initializeImpl/extractImpl with their template-parameterized logic.
class ArchetypeBase : public FeatureExtractorBase {
 public:
  // No-op: registry.cpp calls Derived::setName() but names are now
  // returned directly from name() overrides in each archetype.
  static void setName(const char*) {}

  void initialize(int complexity, int seed) override {
    initializeImpl(complexity + seed);

    // Set up internal feature metadata
    int numFeatures = 50 + complexity * 10;
    features_.resize(numFeatures);
    for (int i = 0; i < numFeatures; ++i) {
      features_[i].raw_feature_index = i % numFeatures;
      features_[i].is_indexed = true;
      features_[i].use_all_indexes = true;
    }
  }

  void extract(
      const std::vector<float>& input_dense,
      const std::vector<int64_t>& input_sparse,
      std::vector<float>& output_dense,
      std::vector<int64_t>& output_sparse) override {
    int numFeatures = static_cast<int>(features_.size());
    example_.resize(
        std::max(numFeatures, static_cast<int>(input_dense.size())),
        std::min(50, numFeatures),
        0);

    // Use input_sparse as query keys, or generate some
    std::vector<int64_t> queryKeys;
    if (!input_sparse.empty()) {
      queryKeys = input_sparse;
    } else {
      queryKeys.resize(20);
      uint64_t state = 42;
      for (auto& k : queryKeys) {
        k = randomInt64(state);
      }
    }

    extractImpl(example_, features_, queryKeys);

    // Copy results to output vectors
    output_dense = example_.denseValues;
    output_sparse.clear();
    for (const auto& list : example_.idScoreLists) {
      for (const auto& pair : list) {
        output_sparse.push_back(pair.id);
      }
    }
  }

  virtual void initializeImpl(int complexity) = 0;
  virtual void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) = 0;

 protected:
  MockFeatureExample example_;
  std::vector<MockFeature> features_;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
