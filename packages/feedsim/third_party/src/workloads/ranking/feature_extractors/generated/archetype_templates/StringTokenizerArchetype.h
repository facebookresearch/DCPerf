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

template <int MaxTokens, int VocabSizeLog2, int TokenizerType, int InputLength>
class StringTokenizerArchetype : public ArchetypeBase {
  static constexpr int kVocabSize = 1 << VocabSizeLog2;
  static constexpr uint64_t kSeed =
      MaxTokens * 100000ULL + VocabSizeLog2 * 1000ULL +
      TokenizerType * 100ULL + InputLength;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int i = 0; i < kVocabSize && i < 8192; ++i) {
      uint64_t key = splitmix64(state);
      vocab_[key] = static_cast<int>(splitmix64(state) % kVocabSize);
    }
    for (int i = 0; i < InputLength; ++i) {
      inputData_[i] = splitmix64(state);
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    int tokenCount = 0;
    for (int pos = 0; pos < InputLength && tokenCount < MaxTokens &&
         featIdx < static_cast<int>(features.size());) {
      int tokenId = tokenize(pos);
      if (tokenId >= 0) {
        yieldIndexedFeature(
            example, features[featIdx], tokenId, 1.0f);
        ++tokenCount;
        ++featIdx;
      }
    }
  }

  std::string name() const override { return "StringTokenizerVariant"; }

 private:
  __attribute__((noinline)) int tokenize(int& pos) {
    if constexpr (TokenizerType == 0) {
      uint64_t key = inputData_[pos];
      ++pos;
      auto it = vocab_.find(key);
      return (it != vocab_.end()) ? it->second : -1;
    } else if constexpr (TokenizerType == 1) {
      if (pos + 1 >= InputLength) { ++pos; return -1; }
      uint64_t key = inputData_[pos] ^ (inputData_[pos + 1] << 7);
      pos += 2;
      auto it = vocab_.find(key);
      return (it != vocab_.end()) ? it->second : static_cast<int>(inputData_[pos > 0 ? pos - 1 : 0] % kVocabSize);
    } else {
      for (int len = std::min(4, InputLength - pos); len >= 1; --len) {
        uint64_t key = 0;
        for (int i = 0; i < len; ++i) {
          key ^= inputData_[pos + i] << (i * 11);
        }
        auto it = vocab_.find(key);
        if (it != vocab_.end()) {
          pos += len;
          return it->second;
        }
      }
      ++pos;
      return static_cast<int>(inputData_[pos > 0 ? pos - 1 : 0] % kVocabSize);
    }
  }

  std::unordered_map<uint64_t, int> vocab_;
  std::array<uint64_t, InputLength> inputData_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
