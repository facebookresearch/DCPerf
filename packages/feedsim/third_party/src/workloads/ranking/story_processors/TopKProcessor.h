// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "StoryPrimitives.h"
#include "StoryProcessorBase.h"

namespace dcperf {
namespace story_processors {

// TopKProcessor — mirrors prod
// `computeTopEventFromScoreInfosTopKOnly` (Utils.cpp:1105) and
// `OrganicStoryRanker::doRegularPass1Ranking` ordering step.
// std::partial_sort of K stories by score. Per-batch CPU ~5 µs at
// K=50 over 100-story input.
class TopKProcessor final : public StoryProcessorBase {
 public:
  void initialize(int complexity, uint64_t seed) override;
  void process(MockStoryList& stories, MockQueryCtx& ctx) override;
  std::string name() const override { return "topk_pass"; }

 private:
  int complexity_ = 5;
};

} // namespace story_processors
} // namespace dcperf
