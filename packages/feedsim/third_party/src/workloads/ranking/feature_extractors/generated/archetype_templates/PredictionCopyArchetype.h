// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <unordered_map>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <int NumPredictions, int LookupType, int HasFallback, int HasCalibration>
class PredictionCopyArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      NumPredictions * 10000ULL + LookupType * 100ULL +
      HasFallback * 10ULL + HasCalibration;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < NumPredictions; ++i) {
      predictions_[i] = randomFloat(state);
      if constexpr (LookupType == 0) {
        predMap_[randomInt64(state)] = predictions_[i];
      }
    }
    for (int i = 0; i < NumPredictions; ++i) {
      lookupKeys_[i] = randomInt64(state);
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int p = 0; p < NumPredictions && featIdx < static_cast<int>(features.size()); ++p) {
      float val = lookupPrediction(p);
      if (val == 0.0f && !HasFallback) {
        ++featIdx;
        continue;
      }
      if (val == 0.0f && HasFallback) {
        val = 0.5f;
      }
      val = calibrate(val);
      yieldScalarFeature(example, features[featIdx], val);
      ++featIdx;
    }
  }

  std::string name() const override { return "PredictionCopyVariant"; }

 private:
  __attribute__((noinline)) float lookupPrediction(int idx) const {
    if constexpr (LookupType == 0) {
      auto it = predMap_.find(lookupKeys_[idx]);
      return (it != predMap_.end()) ? it->second : 0.0f;
    } else if constexpr (LookupType == 1) {
      return predictions_[idx];
    } else {
      int bucket = idx % 4;
      switch (bucket) {
        case 0: return predictions_[idx];
        case 1: return predictions_[idx] * 1.1f;
        case 2: return predictions_[idx] * 0.9f;
        default: return predictions_[idx] + 0.01f;
      }
    }
  }

  __attribute__((noinline)) float calibrate(float val) const {
    if constexpr (HasCalibration == 0) {
      return val;
    } else if constexpr (HasCalibration == 1) {
      return 1.0f / (1.0f + std::exp(-val * 4.0f + 2.0f));
    } else {
      return val * 0.95f + 0.025f;
    }
  }

  std::array<float, NumPredictions> predictions_{};
  std::array<int64_t, NumPredictions> lookupKeys_{};
  std::unordered_map<int64_t, float> predMap_;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
