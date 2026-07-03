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
    int EmbeddingDim,
    int NumEmbeddings,
    int IsSparse,
    int YieldDimensions,
    int SimilarityType>
class EmbeddingDotProductArchetype : public ArchetypeBase {
  static constexpr uint64_t kSeed =
      EmbeddingDim * 10000ULL + NumEmbeddings * 100ULL + IsSparse * 10ULL +
      YieldDimensions * 3ULL + SimilarityType;

 public:
  void initializeImpl(int complexity) override {
    uint64_t state = kSeed + complexity;
    for (int e = 0; e < NumEmbeddings; ++e) {
      for (int d = 0; d < EmbeddingDim; ++d) {
        queryVecs_[e * EmbeddingDim + d] = randomFloat(state) - 0.5f;
        docVecs_[e * EmbeddingDim + d] = randomFloat(state) - 0.5f;
      }
    }
  }

  __attribute__((noinline)) void extractImpl(
      MockFeatureExample& example,
      const std::vector<MockFeature>& features,
      const std::vector<int64_t>& queryKeys) override {
    int featIdx = 0;
    for (int e = 0; e < NumEmbeddings && featIdx < static_cast<int>(features.size()); ++e) {
      float score = computeSimilarity(e);
      yieldScalarFeature(example, features[featIdx], score);
      ++featIdx;

      if constexpr (YieldDimensions) {
        for (int d = 0; d < EmbeddingDim && featIdx < static_cast<int>(features.size()); ++d) {
          float dimVal = queryVecs_[e * EmbeddingDim + d];
          yieldIndexedFeature(example, features[featIdx], d, dimVal);
        }
        ++featIdx;
      }
    }
  }

  std::string name() const override { return "EmbeddingVariant"; }

 private:
  __attribute__((noinline)) float computeSimilarity(int embIdx) const {
    const float* q = &queryVecs_[embIdx * EmbeddingDim];
    const float* d = &docVecs_[embIdx * EmbeddingDim];

    if constexpr (SimilarityType == 0) {
      float dot = 0.0f;
      for (int i = 0; i < EmbeddingDim; ++i) {
        dot += q[i] * d[i];
      }
      return dot;
    } else if constexpr (SimilarityType == 1) {
      float dot = 0.0f, normQ = 0.0f, normD = 0.0f;
      for (int i = 0; i < EmbeddingDim; ++i) {
        dot += q[i] * d[i];
        normQ += q[i] * q[i];
        normD += d[i] * d[i];
      }
      float denom = std::sqrt(normQ * normD);
      return denom > 1e-8f ? dot / denom : 0.0f;
    } else {
      float dist = 0.0f;
      for (int i = 0; i < EmbeddingDim; ++i) {
        float diff = q[i] - d[i];
        dist += diff * diff;
      }
      return 1.0f / (1.0f + std::sqrt(dist));
    }
  }

  std::array<float, EmbeddingDim * NumEmbeddings> queryVecs_{};
  std::array<float, EmbeddingDim * NumEmbeddings> docVecs_{};
};

} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
