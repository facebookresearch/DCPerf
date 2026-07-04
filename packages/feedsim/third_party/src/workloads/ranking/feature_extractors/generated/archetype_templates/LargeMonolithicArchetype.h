// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <map>
#include <unordered_map>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int NumFeatures,
    int NumCodePaths,
    int PathComplexity,
    int NumHashLookups,
    int NumNestedLoops>
class LargeMonolithicArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      NumFeatures * 100000ULL + NumCodePaths * 1000ULL +
      PathComplexity * 100ULL + NumHashLookups * 10ULL + NumNestedLoops;
  static constexpr int kTableSize = 512;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < kTableSize; ++i) {
      hashTable_[randomInt64(state)] = randomFloat(state);
    }
    for (int i = 0; i < 256; ++i) {
      sortedMap_[randomInt64(state)] = randomFloat(state);
    }
    for (int i = 0; i < NumFeatures; ++i) {
      coefficients_[i] = randomFloat(state) - 0.5f;
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;

    phaseHashLookup(example, features, queryKeys, featIdx);
    phaseCompute(example, features, queryKeys, featIdx);
    phaseTraverse(example, features, featIdx);
  }

  std::string name() const override { return "LargeMonolithicVariant"; }

 private:
  __attribute__((noinline)) void phaseHashLookup(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys,
      int& featIdx) {
    for (int h = 0; h < NumHashLookups && featIdx < static_cast<int>(features.size()); ++h) {
      int64_t key = queryKeys[h % queryKeys.size()];
      auto it = hashTable_.find(key);
      float val = (it != hashTable_.end()) ? it->second : coefficients_[h % NumFeatures];
      yieldScalarFeature(example, features[featIdx], val);
      ++featIdx;
    }
  }

  __attribute__((noinline)) void phaseCompute(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys,
      int& featIdx) {
    for (int p = 0; p < NumCodePaths && featIdx < static_cast<int>(features.size()); ++p) {
      float result = 0.0f;
      if constexpr (PathComplexity == 0) {
        result = coefficients_[p % NumFeatures];
      } else if constexpr (PathComplexity == 1) {
        for (int i = 0; i < NumNestedLoops * 3; ++i) {
          result += coefficients_[(p + i) % NumFeatures] *
              static_cast<float>(i + 1);
        }
        result /= static_cast<float>(NumNestedLoops * 3);
      } else {
        for (int i = 0; i < NumNestedLoops; ++i) {
          for (int j = 0; j < NumNestedLoops + 2; ++j) {
            int idx = (p * NumNestedLoops + i * (NumNestedLoops + 2) + j) %
                NumFeatures;
            float c = coefficients_[idx];
            result += c * c - c * 0.5f;
          }
        }
        result = std::tanh(result);
      }

      if (p % 4 == 0) {
        yieldScalarFeature(example, features[featIdx], result);
      } else if (p % 4 == 1) {
        yieldScalarFeature(example, features[featIdx], std::abs(result));
      } else if (p % 4 == 2) {
        yieldScalarFeature(example, features[featIdx], result > 0.0f ? result : 0.0f);
      } else {
        yieldScalarFeature(example, features[featIdx], std::log1p(std::abs(result)));
      }
      ++featIdx;
    }
  }

  __attribute__((noinline)) void phaseTraverse(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      int& featIdx) {
    int remaining = NumFeatures - NumHashLookups - NumCodePaths;
    int count = 0;
    for (auto it = sortedMap_.begin();
         it != sortedMap_.end() && count < remaining &&
         featIdx < static_cast<int>(features.size());
         ++it, ++count) {
      yieldIndexedFeature(
          example, features[featIdx], it->first, it->second);
      ++featIdx;
    }
  }

  std::unordered_map<int64_t, float> hashTable_;
  std::map<int64_t, float> sortedMap_;
  std::array<float, NumFeatures> coefficients_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
