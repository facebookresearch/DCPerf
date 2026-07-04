// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <int NumSparseLists, int ItemsPerList, int HasFilter, int FilterStyle>
class SparseForwardArchetype : public ArchetypeBase {
  static constexpr int kTotalItems = NumSparseLists * ItemsPerList;
  static constexpr uint64_t kSeed =
      NumSparseLists * 10000ULL + ItemsPerList * 100ULL +
      HasFilter * 10ULL + FilterStyle;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < kTotalItems; ++i) {
      sparseData_[i] = {randomInt64(state), randomFloat(state)};
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int l = 0; l < NumSparseLists && featIdx < static_cast<int>(features.size()); ++l) {
      int base = l * ItemsPerList;
      for (int i = 0; i < ItemsPerList; ++i) {
        const auto& pair = sparseData_[base + i];
        if constexpr (HasFilter) {
          if (!passesFilter(pair.score, i)) continue;
        }
        yieldIndexedFeature(
            example, features[featIdx], pair.id, pair.score);
      }
      ++featIdx;
    }
  }

  std::string name() const override { return "SparseForwardVariant"; }

 private:
  __attribute__((noinline)) bool passesFilter(float score, int pos) const {
    if constexpr (FilterStyle == 0) {
      return score > 0.1f;
    } else if constexpr (FilterStyle == 1) {
      return pos < ItemsPerList / 2;
    } else {
      return std::abs(score - 0.5f) > 0.2f;
    }
  }

  std::array<IdScorePair, kTotalItems> sparseData_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
