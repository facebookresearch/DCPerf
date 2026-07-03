// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <cmath>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int NumRateSlots,
    int NumCounterSlots,
    int NumDimensions,
    int ImpBucketType,
    int HasRateFeatures>
class RateCounterArchetype : public ArchetypeBase {
  static constexpr int kTotalSlots =
      (HasRateFeatures ? NumRateSlots : 0) + NumCounterSlots;
  static constexpr uint64_t kSeed =
      NumRateSlots * 10000ULL + NumCounterSlots * 100ULL +
      NumDimensions * 10ULL + ImpBucketType * 3ULL + HasRateFeatures;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int d = 0; d < NumDimensions; ++d) {
      for (int s = 0; s < NumRateSlots; ++s) {
        rates_[d * NumRateSlots + s] = randomFloat(state);
      }
      for (int s = 0; s < NumCounterSlots; ++s) {
        counters_[d * NumCounterSlots + s] = randomInt64(state) & 0xFFFF;
      }
    }
    impressions_ = 100 + (complexity * 17);
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int d = 0; d < NumDimensions; ++d) {
      if constexpr (HasRateFeatures) {
        for (int s = 0; s < NumRateSlots && featIdx < static_cast<int>(features.size()); ++s) {
          float rate = rates_[d * NumRateSlots + s];
          if (rate != 0.0f) {
            float bucketed = bucketRate(rate, impressions_);
            yieldScalarFeature(example, features[featIdx], bucketed);
          }
          ++featIdx;
        }
      }
      for (int s = 0; s < NumCounterSlots && featIdx < static_cast<int>(features.size()); ++s) {
        int64_t count = counters_[d * NumCounterSlots + s];
        if (count > 0) {
          float val = static_cast<float>(count);
          yieldScalarFeature(example, features[featIdx], val);
        }
        ++featIdx;
      }
    }
  }

  std::string name() const override { return "RateCounterVariant"; }

 private:
  __attribute__((noinline)) float bucketRate(float rate, int impressions) const {
    if constexpr (ImpBucketType == 0) {
      return rate * static_cast<float>(impressions) / 100.0f;
    } else if constexpr (ImpBucketType == 1) {
      float logImp = std::log2(static_cast<float>(impressions + 1));
      return rate * logImp;
    } else {
      float imp = static_cast<float>(impressions);
      if (imp < 10.0f) return rate * 0.1f;
      if (imp < 100.0f) return rate * 0.5f;
      if (imp < 1000.0f) return rate * 0.9f;
      return rate;
    }
  }

  std::array<float, NumRateSlots * NumDimensions> rates_{};
  std::array<int64_t, NumCounterSlots * NumDimensions> counters_{};
  int impressions_ = 0;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
