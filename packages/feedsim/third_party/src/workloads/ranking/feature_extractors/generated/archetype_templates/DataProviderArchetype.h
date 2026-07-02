// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <cstring>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int NumFields,
    int NumBranches,
    int FieldTypeSet,
    int DataSource,
    int HasCrossFeatures>
class DataProviderArchetype : public ArchetypeBase {
  static constexpr int kTotalFields =
      NumFields + (HasCrossFeatures ? NumFields / 2 : 0);
  static constexpr uint64_t kSeed =
      NumFields * 10000ULL + NumBranches * 1000ULL + FieldTypeSet * 100ULL +
      DataSource * 10ULL + HasCrossFeatures;
  static constexpr int kStructSize = 64 + DataSource * 32;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < kStructSize; ++i) {
      structData_[i] = randomFloat(state);
    }
    for (int i = 0; i < NumFields; ++i) {
      fieldOffsets_[i] = static_cast<int>(splitmix64(state) % kStructSize);
      fieldValid_[i] = (splitmix64(state) % 10) > 2;
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int f = 0; f < NumFields && featIdx < static_cast<int>(features.size()); ++f) {
      bool valid = true;
      for (int b = 0; b < NumBranches; ++b) {
        if (!fieldValid_[(f + b) % NumFields]) {
          valid = false;
          break;
        }
      }
      if (valid) {
        float val = readField(f);
        yieldScalarFeature(example, features[featIdx], val);
      }
      ++featIdx;
    }

    if constexpr (HasCrossFeatures) {
      computeCrossFeatures(example, features, featIdx);
    }
  }

  std::string name() const override { return "DataProviderVariant"; }

 private:
  __attribute__((noinline)) float readField(int fieldIdx) const {
    float raw = structData_[fieldOffsets_[fieldIdx]];
    if constexpr (FieldTypeSet == 0) {
      return raw;
    } else if constexpr (FieldTypeSet == 1) {
      return raw > 0.5f ? 1.0f : 0.0f;
    } else if constexpr (FieldTypeSet == 2) {
      return static_cast<float>(static_cast<int>(raw * 10.0f));
    } else {
      if (fieldIdx & 1) return raw;
      return static_cast<float>(static_cast<int>(raw * 100.0f)) / 100.0f;
    }
  }

  __attribute__((noinline)) void computeCrossFeatures(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      int& featIdx) {
    for (int i = 0; i < NumFields / 2 && featIdx < static_cast<int>(features.size()); ++i) {
      float v1 = structData_[fieldOffsets_[i]];
      float v2 = structData_[fieldOffsets_[(i + 1) % NumFields]];
      float cross = v1 * v2;
      yieldScalarFeature(example, features[featIdx], cross);
      ++featIdx;
    }
  }

  std::array<float, kStructSize> structData_{};
  std::array<int, NumFields> fieldOffsets_{};
  std::array<bool, NumFields> fieldValid_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
