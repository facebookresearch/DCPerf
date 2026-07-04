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

template <
    int NumTables,
    int TableSizeLog2,
    int LookupsPerTable,
    int BranchPattern,
    int YieldStyle>
class HashLookupArchetype : public ArchetypeBase {
  static constexpr int kTableSize = 1 << TableSizeLog2;
  static constexpr uint64_t kSeed =
      NumTables * 100000ULL + TableSizeLog2 * 1000ULL +
      LookupsPerTable * 10ULL + BranchPattern * 3ULL + YieldStyle;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int t = 0; t < NumTables; ++t) {
      for (int i = 0; i < kTableSize; ++i) {
        int64_t key = randomInt64(state);
        float val = randomFloat(state);
        tables_[t][key] = val;
      }
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int t = 0; t < NumTables; ++t) {
      for (int i = 0; i < LookupsPerTable; ++i) {
        if (featIdx >= static_cast<int>(features.size())) return;
        int64_t key = deriveKey(queryKeys, t, i);
        auto it = tables_[t].find(key);
        float val = (it != tables_[t].end()) ? it->second : 0.0f;
        yieldResult(example, features, featIdx, key, val, t, i);
        ++featIdx;
      }
    }
  }

  std::string name() const override { return "HashLookupVariant"; }

 private:
  __attribute__((noinline)) int64_t
  deriveKey(const std::vector<int64_t>& keys, int table, int idx) const {
    int64_t base = keys[idx % keys.size()];
    if constexpr (BranchPattern == 0) {
      return base ^ (static_cast<int64_t>(table) * 0x9e3779b97f4a7c15LL);
    } else if constexpr (BranchPattern == 1) {
      return base * (2654435761LL + table * 7) + idx * 31;
    } else if constexpr (BranchPattern == 2) {
      int64_t h = base;
      h ^= h >> 16;
      h *= 0x45d9f3b + table;
      h ^= h >> 16;
      return h + idx;
    } else {
      int64_t h1 = base ^ (base >> 32);
      int64_t h2 = h1 * (0xbf58476d1ce4e5b9LL + table);
      return h2 ^ (h2 >> 31) ^ idx;
    }
  }

  __attribute__((noinline)) void yieldResult(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      int featIdx,
      int64_t key,
      float val,
      int table,
      int idx) {
    if constexpr (YieldStyle == 0) {
      yieldIndexedFeature(example, features[featIdx], key, val);
    } else if constexpr (YieldStyle == 1) {
      float bucketedVal = val * (1.0f + static_cast<float>(table) * 0.1f);
      yieldIndexedFeature(
          example, features[featIdx], key & 0xFF, bucketedVal);
    } else {
      float crossVal = val * static_cast<float>(idx + 1) /
          static_cast<float>(LookupsPerTable);
      int64_t crossKey = key ^ (static_cast<int64_t>(table) << 32);
      yieldIndexedFeature(example, features[featIdx], crossKey, crossVal);
    }
  }

  std::array<std::unordered_map<int64_t, float>, NumTables> tables_;
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
