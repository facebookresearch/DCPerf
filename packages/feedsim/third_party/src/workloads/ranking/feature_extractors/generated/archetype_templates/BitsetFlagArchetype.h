// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "ArchetypeBase.h"
#include <array>
#include <vector>

namespace dcperf {
namespace feature_extractors {
namespace generated {

template <
    int BitsetSizeLog2,
    int ChecksPerStory,
    int HasTypeConversion,
    int ConversionCount>
class BitsetFlagArchetype : public ArchetypeBase {
  static constexpr int kBitsetSize = 1 << BitsetSizeLog2;
  static constexpr int kWords = (kBitsetSize + 63) / 64;
  static constexpr uint64_t kSeed =
      BitsetSizeLog2 * 10000ULL + ChecksPerStory * 10ULL +
      HasTypeConversion * 3ULL + ConversionCount;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int w = 0; w < kWords; ++w) {
      bitWords_[w] = splitmix64(state);
    }
    for (int i = 0; i < ChecksPerStory; ++i) {
      checkIndices_[i] = splitmix64(state) % kBitsetSize;
    }
    for (int i = 0; i < kBitsetSize; ++i) {
      values_[i] = randomFloat(state) * 100.0 - 50.0;
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int c = 0; c < ChecksPerStory && featIdx < static_cast<int>(features.size()); ++c) {
      int bitIdx = checkIndices_[c];
      bool isSet = (bitWords_[bitIdx / 64] >> (bitIdx % 64)) & 1;
      if (!isSet) continue;

      double rawVal = values_[bitIdx];
      float val;
      if constexpr (HasTypeConversion) {
        val = cappedConvert<float>(rawVal);
      } else {
        val = static_cast<float>(rawVal);
      }
      yieldIndexedFeature(
          example, features[featIdx], bitIdx, val);
      ++featIdx;
    }
  }

  std::string name() const override { return "BitsetFlagVariant"; }

 private:
  std::array<uint64_t, kWords> bitWords_{};
  std::array<int, ChecksPerStory> checkIndices_{};
  std::array<double, kBitsetSize> values_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
