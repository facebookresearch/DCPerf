// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "StoryPrimitives.h"
#include "StoryProcessorBase.h"

namespace dcperf {
namespace story_processors {

// BlendingPassProcessor — mirrors prod
// `BlenderPass::blendStories` + `MinGapRuleEnforcementComponent::enforce`.
// 2-way merge of two sorted-by-score story lists with a min-gap
// state-machine that prevents adjacent stories of the same source_type
// within a configurable window. Per-blend CPU ~400 ns at K=50 stories.
class BlendingPassProcessor final : public StoryProcessorBase {
 public:
  void initialize(int complexity, uint64_t seed) override;
  void process(MockStoryList& stories, MockQueryCtx& ctx) override;
  std::string name() const override { return "blending_pass"; }

 private:
  int complexity_ = 5;
  MockStoryList aux_list_; // pre-allocated synthetic list for merge
};

} // namespace story_processors
} // namespace dcperf
