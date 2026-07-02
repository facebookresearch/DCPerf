// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <vector>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int InlineSize,
    int ItemsPerCollection,
    int NumCollections,
    int HasDedup,
    int GrowthPattern>
class ContainerCollectArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      InlineSize * 10000ULL + ItemsPerCollection * 100ULL +
      NumCollections * 10ULL + HasDedup * 3ULL + GrowthPattern;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int c = 0; c < NumCollections; ++c) {
      for (int i = 0; i < ItemsPerCollection; ++i) {
        srcIds_[c * ItemsPerCollection + i] = randomInt64(state);
        srcScores_[c * ItemsPerCollection + i] = randomFloat(state);
      }
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int c = 0; c < NumCollections && featIdx < static_cast<int>(features.size()); ++c) {
      std::vector<IdScorePair> collected;
      collectItems(collected, c);

      for (const auto& pair : collected) {
        if (featIdx >= static_cast<int>(features.size())) break;
        yieldIndexedFeature(
            example, features[featIdx], pair.id, pair.score);
      }
      ++featIdx;
    }
  }

  std::string name() const override { return "ContainerCollectVariant"; }

 private:
  __attribute__((noinline)) void
  collectItems(std::vector<IdScorePair>& out, int collIdx) {
    int base = collIdx * ItemsPerCollection;
    if constexpr (GrowthPattern == 0) {
      for (int i = 0; i < ItemsPerCollection; ++i) {
        out.emplace_back(
            IdScorePair{srcIds_[base + i], srcScores_[base + i]});
      }
    } else if constexpr (GrowthPattern == 1) {
      for (int i = 0; i < ItemsPerCollection; ++i) {
        if (srcScores_[base + i] > 0.2f) {
          out.emplace_back(
              IdScorePair{srcIds_[base + i], srcScores_[base + i]});
        }
      }
    } else {
      for (int i = 0; i < ItemsPerCollection; ++i) {
        float score = srcScores_[base + i];
        if (score > 0.1f && score < 0.9f) {
          int64_t id = srcIds_[base + i];
          if constexpr (HasDedup) {
            bool dup = false;
            for (const auto& p : out) {
              if (p.id == id) { dup = true; break; }
            }
            if (dup) continue;
          }
          out.emplace_back(IdScorePair{id, score});
        }
      }
    }
  }

  std::array<int64_t, ItemsPerCollection * NumCollections> srcIds_{};
  std::array<float, ItemsPerCollection * NumCollections> srcScores_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
