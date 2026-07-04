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

template <int NumDenseRemap, int NumSparseRemap, int ArraySizeLog2>
class FeatureLayoutCopyArchetype : public ArchetypeBase {
  static constexpr int kArraySize = 1 << ArraySizeLog2;
  static constexpr uint64_t kSeed =
      NumDenseRemap * 10000ULL + NumSparseRemap * 100ULL + ArraySizeLog2;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < NumDenseRemap; ++i) {
      denseRemapTo_[i] = splitmix64(state) % kArraySize;
      denseRemapFrom_[i] = splitmix64(state) % kArraySize;
    }
    for (int i = 0; i < NumSparseRemap; ++i) {
      sparseRemapTo_[i] = splitmix64(state) % kArraySize;
      sparseRemapFrom_[i] = splitmix64(state) % kArraySize;
    }
    for (int i = 0; i < kArraySize; ++i) {
      cacheArray_[i] = randomFloat(state);
    }
    for (int i = 0; i < NumSparseRemap * 5; ++i) {
      cacheSparse_[i] = {randomInt64(state), randomFloat(state)};
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    // Dense remap: indirect-indexed copy
    if (static_cast<int>(example.denseValues.size()) >= kArraySize) {
      for (int i = 0; i < NumDenseRemap; ++i) {
        example.denseValues[denseRemapTo_[i]] =
            cacheArray_[denseRemapFrom_[i]];
      }
    }

    // Sparse remap: vector assignment
    int sparseLimit = std::min(
        NumSparseRemap,
        static_cast<int>(example.idScoreLists.size()));
    for (int i = 0; i < sparseLimit; ++i) {
      int from = sparseRemapFrom_[i] % (NumSparseRemap * 5);
      auto& dst = example.idScoreLists[sparseRemapTo_[i] %
          example.idScoreLists.size()];
      dst.clear();
      for (int j = from; j < from + 3 && j < NumSparseRemap * 5; ++j) {
        dst.push_back(cacheSparse_[j]);
      }
    }
  }

  std::string name() const override { return "FeatureLayoutCopyVariant"; }

 private:
  std::array<int, NumDenseRemap> denseRemapTo_{};
  std::array<int, NumDenseRemap> denseRemapFrom_{};
  std::array<int, NumSparseRemap> sparseRemapTo_{};
  std::array<int, NumSparseRemap> sparseRemapFrom_{};
  std::array<float, kArraySize> cacheArray_{};
  std::array<IdScorePair, NumSparseRemap * 5> cacheSparse_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
