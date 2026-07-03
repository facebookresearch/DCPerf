// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "../../FeatureTypes.h"
#include <cstdint>

namespace dcperf {
namespace feature_extractors {
namespace generated {

// splitmix64 PRNG for deterministic initialization
inline uint64_t splitmix64(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

inline int64_t randomInt64(uint64_t& state) {
  return static_cast<int64_t>(splitmix64(state));
}

inline float randomFloat(uint64_t& state) {
  return static_cast<float>(splitmix64(state) & 0xFFFFFF) / 16777216.0f;
}

// Scalar feature yield (stores as dense value)
inline void yieldScalarFeature(
    MockFeatureExample& example,
    const MockFeature& feature,
    float val) {
  int32_t idx = feature.raw_feature_index;
  if (idx >= 0 && idx < static_cast<int32_t>(example.denseValues.size())) {
    example.denseValues[idx] = val;
  }
}

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
