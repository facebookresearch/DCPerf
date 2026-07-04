// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <map>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <int MapSizeLog2, int MapsPerStory, int HasRangeFilter, int ValueStyle>
class TreeMapArchetype : public ArchetypeBase {
  static constexpr int kMapSize = 1 << MapSizeLog2;
  static constexpr uint64_t kSeed =
      MapSizeLog2 * 1000ULL + MapsPerStory * 100ULL + HasRangeFilter * 10ULL +
      ValueStyle;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int m = 0; m < MapsPerStory; ++m) {
      for (int i = 0; i < kMapSize; ++i) {
        int64_t key = randomInt64(state);
        float val = randomFloat(state);
        maps_[m][key] = val;
      }
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int m = 0; m < MapsPerStory; ++m) {
      if constexpr (HasRangeFilter) {
        int64_t lb = queryKeys.empty() ? 0 : queryKeys[0];
        auto it = maps_[m].lower_bound(lb);
        int count = 0;
        while (it != maps_[m].end() && count < kMapSize / 2 &&
               featIdx < static_cast<int>(features.size())) {
          float val = transformValue(it->second, count);
          yieldIndexedFeature(
              example, features[featIdx], it->first, val);
          ++it;
          ++count;
          ++featIdx;
        }
      } else {
        int count = 0;
        for (const auto& [key, value] : maps_[m]) {
          if (featIdx >= static_cast<int>(features.size())) break;
          float val = transformValue(value, count);
          yieldIndexedFeature(example, features[featIdx], key, val);
          ++count;
          ++featIdx;
        }
      }
    }
  }

  std::string name() const override { return "TreeMapVariant"; }

 private:
  __attribute__((noinline)) float transformValue(float raw, int pos) const {
    if constexpr (ValueStyle == 0) {
      return raw;
    } else if constexpr (ValueStyle == 1) {
      return raw * (1.0f + pos * 0.01f);
    } else {
      if (raw < 0.25f) return 0.1f;
      if (raw < 0.5f) return 0.3f;
      if (raw < 0.75f) return 0.7f;
      return 1.0f;
    }
  }

  std::array<std::map<int64_t, float>, MapsPerStory> maps_;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
