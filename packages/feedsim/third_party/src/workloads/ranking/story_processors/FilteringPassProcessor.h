// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "StoryPrimitives.h"
#include "StoryProcessorBase.h"

namespace dcperf {
namespace story_processors {

// FilteringPassProcessor — mirrors prod
// `OrganicStoryRanker::applyFilteringBeforePass0` +
// `BadObjectFilterUtils::filterBadObjects`. In-place vector compaction
// where each story is checked against a predicate set; failed entries
// are dropped (std::erase_if-like). Per-story CPU ~200 ns.
class FilteringPassProcessor final : public StoryProcessorBase {
 public:
  void initialize(int complexity, uint64_t seed) override;
  void process(MockStoryList& stories, MockQueryCtx& ctx) override;
  std::string name() const override { return "filtering_pass"; }

 private:
  int complexity_ = 5;
  // Per-thread monotonic predicate flag set seed. Used to vary the
  // filter-keep ratio across requests so the working set doesn't get
  // pinned to a single shape.
  uint64_t filter_seed_ = 0;
};

} // namespace story_processors
} // namespace dcperf
