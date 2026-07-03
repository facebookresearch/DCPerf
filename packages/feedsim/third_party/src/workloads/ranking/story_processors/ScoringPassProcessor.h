// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <unordered_map>
#include <vector>

#include "StoryPrimitives.h"
#include "StoryProcessorBase.h"

namespace dcperf {
namespace story_processors {

// ScoringPassProcessor — mirrors prod multifeed
// `ValueModelApplier::calculateStoryScores` + `ValueModel::getValues` +
// `FastBooleanTest::passes` workload. Per story: F14-style hash lookups
// of `score_keys[]` against a per-thread weight table, a small numeric
// reduce, and a boolean predicate evaluation. Target per-story CPU
// ~1200 ns to match prod's ~14% Ranking-Story scoring share.
class ScoringPassProcessor final : public StoryProcessorBase {
 public:
  void initialize(int complexity, uint64_t seed) override;
  void process(MockStoryList& stories, MockQueryCtx& ctx) override;
  std::string name() const override { return "scoring_pass"; }

 private:
  std::unordered_map<int64_t, float> weight_table_;
  std::vector<float> bias_table_;
  int complexity_ = 5;
};

} // namespace story_processors
} // namespace dcperf
