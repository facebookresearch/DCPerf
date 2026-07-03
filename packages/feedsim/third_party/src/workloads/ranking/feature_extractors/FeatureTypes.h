// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// Simplified feature metadata
struct MockFeature {
  int32_t raw_feature_index = -1;
  bool is_indexed = true;
  bool use_all_indexes = true;
};

// Output pair for sparse features
struct IdScorePair {
  int64_t id;
  float score;
};

// Feature example buffer filled during extraction
struct MockFeatureExample {
  std::vector<float> denseValues;
  std::vector<std::vector<IdScorePair>> idScoreLists;
  std::vector<int64_t> intValues;

  void resize(int numDense, int numSparse, int numInt) {
    denseValues.assign(numDense, 0.0f);
    idScoreLists.resize(numSparse);
    for (auto& v : idScoreLists) {
      v.clear();
    }
    intValues.assign(numInt, 0);
  }
};

// Float validation matching production's FloatUtils::isFiniteAndNonZero
inline bool isFiniteAndNonZero(float val) {
  return val != 0.0f && std::isfinite(val);
}

// 3-level yield pipeline matching production dispatch overhead
inline void consumeIndexedFeature(
    MockFeatureExample& example,
    const MockFeature& feature,
    int64_t index,
    float val) {
  int32_t idx = feature.raw_feature_index;
  if (feature.use_all_indexes && idx >= 0 &&
      idx < static_cast<int32_t>(example.idScoreLists.size())) {
    example.idScoreLists[idx].emplace_back(IdScorePair{index, val});
  }
}

inline void yieldIndexedFeature(
    MockFeatureExample& example,
    const MockFeature& feature,
    int64_t index,
    float val) {
  if (__builtin_expect(isFiniteAndNonZero(val), 1)) {
    if (feature.raw_feature_index == -1) {
      return;
    }
    consumeIndexedFeature(example, feature, index, val);
  }
}

// Capped type conversion matching production's cappedTypeConversion
template <typename Tgt, typename Src>
inline Tgt cappedConvert(Src val) {
  if (val > static_cast<Src>(std::numeric_limits<Tgt>::max())) {
    return std::numeric_limits<Tgt>::max();
  }
  if (val < static_cast<Src>(std::numeric_limits<Tgt>::lowest())) {
    return std::numeric_limits<Tgt>::lowest();
  }
  return static_cast<Tgt>(val);
}
