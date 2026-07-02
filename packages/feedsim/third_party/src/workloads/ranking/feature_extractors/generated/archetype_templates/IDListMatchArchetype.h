// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <unordered_set>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int ListSize,
    int MatchType,
    int FeaturesOnMatch,
    int HasRecencyDecay,
    int IdDerivation>
class IDListMatchArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      ListSize * 10000ULL + MatchType * 1000ULL + FeaturesOnMatch * 100ULL +
      HasRecencyDecay * 10ULL + IdDerivation;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < ListSize; ++i) {
      recentIds_[i] = randomInt64(state);
    }
    if constexpr (MatchType == 1) {
      idSet_.insert(recentIds_.begin(), recentIds_.end());
    } else if constexpr (MatchType == 2) {
      std::sort(recentIds_.begin(), recentIds_.end());
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (size_t q = 0; q < queryKeys.size() && featIdx < static_cast<int>(features.size()); ++q) {
      int64_t storyId = deriveId(queryKeys[q]);
      int matchPos = findMatch(storyId);
      if (matchPos >= 0) {
        emitMatchFeatures(example, features, featIdx, matchPos);
      }
    }
  }

  std::string name() const override { return "IDListMatchVariant"; }

 private:
  __attribute__((noinline)) int64_t deriveId(int64_t raw) const {
    if constexpr (IdDerivation == 0) {
      return raw;
    } else if constexpr (IdDerivation == 1) {
      return raw ^ (raw >> 16);
    } else {
      return (raw >> 8) | (raw << 56);
    }
  }

  __attribute__((noinline)) int findMatch(int64_t id) const {
    if constexpr (MatchType == 0) {
      for (int i = 0; i < ListSize; ++i) {
        if (recentIds_[i] == id) return i;
      }
      return -1;
    } else if constexpr (MatchType == 1) {
      return idSet_.count(id) ? 0 : -1;
    } else {
      auto it = std::lower_bound(recentIds_.begin(), recentIds_.end(), id);
      if (it != recentIds_.end() && *it == id) {
        return static_cast<int>(it - recentIds_.begin());
      }
      return -1;
    }
  }

  __attribute__((noinline)) void emitMatchFeatures(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      int& featIdx,
      int pos) {
    for (int f = 0; f < FeaturesOnMatch && featIdx < static_cast<int>(features.size()); ++f) {
      float val = 1.0f;
      if constexpr (HasRecencyDecay) {
        val = 1.0f / (1.0f + static_cast<float>(pos) * 0.1f);
      }
      val *= (1.0f + f * 0.2f);
      yieldScalarFeature(example, features[featIdx], val);
      ++featIdx;
    }
  }

  std::array<int64_t, ListSize> recentIds_{};
  std::unordered_set<int64_t> idSet_;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
