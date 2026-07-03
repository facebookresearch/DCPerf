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

template <
    int WindowId,
    int DimensionId,
    int NumStats,
    int BreakdownType,
    int StatTransform>
class TimeWindowArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      WindowId * 100000ULL + DimensionId * 10000ULL + NumStats * 100ULL +
      BreakdownType * 10ULL + StatTransform;
  static constexpr float kWindowMultiplier =
      WindowId == 0 ? 1.0f :
      WindowId == 1 ? 6.0f :
      WindowId == 2 ? 24.0f :
      WindowId == 3 ? 72.0f :
      WindowId == 4 ? 168.0f : 336.0f;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < NumStats; ++i) {
      stats_[i] = randomFloat(state) * kWindowMultiplier;
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int s = 0; s < NumStats && featIdx < static_cast<int>(features.size()); ++s) {
      float stat = stats_[s];
      if (stat == 0.0f) {
        ++featIdx;
        continue;
      }
      float val = transformStat(stat, s);
      if constexpr (BreakdownType == 0) {
        yieldScalarFeature(example, features[featIdx], val);
      } else if constexpr (BreakdownType == 1) {
        float broken = val / (1.0f + static_cast<float>(DimensionId) * 0.1f);
        yieldScalarFeature(example, features[featIdx], broken);
      } else {
        float uih = val * (1.0f + std::log1p(static_cast<float>(s)));
        yieldScalarFeature(example, features[featIdx], uih);
      }
      ++featIdx;
    }
  }

  std::string name() const override { return "TimeWindowVariant"; }

 private:
  __attribute__((noinline)) float transformStat(float stat, int idx) const {
    if constexpr (StatTransform == 0) {
      return stat;
    } else if constexpr (StatTransform == 1) {
      return stat / (kWindowMultiplier + 1.0f);
    } else {
      return std::log1p(stat) * (1.0f + idx * 0.01f);
    }
  }

  std::array<float, NumStats> stats_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
