// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ScoringPassProcessor.h"

#include <cmath>
#include <random>

namespace dcperf {
namespace story_processors {

namespace {
// FastBooleanTest analog — three-way decision on score + flags.
// Cheap: a couple of branches + one fp compare. Maps to prod's
// FastBooleanTest::passes ~1% hot leaf.
inline bool passesQualityGate(float score, uint32_t flags) {
  if ((flags & 0x1u) == 0u) {
    return false;
  }
  if (score < 0.01f) {
    return false;
  }
  return ((flags & 0x4u) != 0u) || (score > 0.5f);
}
} // namespace

void ScoringPassProcessor::initialize(int complexity, uint64_t seed) {
  complexity_ = complexity;
  // Build a weight table sized by complexity. Higher complexity → more
  // unique keys in flight → more F14 lookup work per story score (more
  // cache misses, slower findImpl). Capped at 64k to keep working set
  // bounded in L2-L3.
  const size_t table_size = static_cast<size_t>(2048) * complexity;
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int64_t> key_dist(0, (1LL << 24) - 1);
  std::uniform_real_distribution<float> w_dist(-1.0f, 1.0f);
  weight_table_.reserve(table_size);
  for (size_t i = 0; i < table_size; ++i) {
    weight_table_.emplace(key_dist(rng), w_dist(rng));
  }
  // Per-story-type bias table (small, fits in L1).
  bias_table_.resize(32);
  for (auto& b : bias_table_) {
    b = w_dist(rng);
  }
}

void ScoringPassProcessor::process(MockStoryList& stories, MockQueryCtx& ctx) {
  // Per-story numeric reduce: lookup each score key in the weight
  // table, multiply-accumulate into the running score, mix in the
  // per-story-type bias, then run a boolean predicate. Mirrors
  // ValueModel::getValues + FastBooleanTest::passes inner loop.
  const float user_factor = 1.0f + (static_cast<float>(ctx.user_id & 0xff) / 256.0f);
  for (auto& s : stories) {
    float acc = s.weight;
    // Cap the loop at the smaller of the two parallel vectors so a
    // malformed MockStory (raw_scores shorter than score_keys) can't
    // OOB-read raw_scores.
    const size_t n = std::min(s.score_keys.size(), s.raw_scores.size());
    for (size_t i = 0; i < n; ++i) {
      auto it = weight_table_.find(s.score_keys[i]);
      const float w = (it != weight_table_.end()) ? it->second : 0.0f;
      acc += w * s.raw_scores[i];
    }
    const size_t bias_idx =
        static_cast<size_t>(s.story_type) & (bias_table_.size() - 1);
    acc = acc * user_factor + bias_table_[bias_idx];
    s.score = acc;
    s.score_info.weight = acc;
    s.score_info.weight_user = acc * user_factor;
    s.score_info.discounted_weight =
        std::tanh(acc) * (passesQualityGate(acc, s.flags) ? 1.0f : 0.5f);
    // Stash flag bit for filter pass.
    if (!passesQualityGate(acc, s.flags)) {
      s.flags |= 0x100u; // mark as filter-candidate
    }
  }
}

} // namespace story_processors
} // namespace dcperf
