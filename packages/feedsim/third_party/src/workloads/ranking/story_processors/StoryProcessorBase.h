// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <string>

#include "StoryPrimitives.h"

namespace dcperf {
namespace story_processors {

// Pipeline-stage interface. Each archetype implements one pass over a
// MockStoryList in-place; stages run in fixed order inside
// StoryProcessorSuite::runOnePipelinePass.
class StoryProcessorBase {
 public:
  virtual ~StoryProcessorBase() = default;

  // Initialize internal hash tables / lookup state. Seed is per-thread.
  virtual void initialize(int complexity, uint64_t seed) = 0;

  // Run this stage on the given story list. Stages may mutate the list
  // (filter shrinks, blend extends, scoring fills scores, etc.).
  virtual void process(MockStoryList& stories, MockQueryCtx& ctx) = 0;

  virtual std::string name() const = 0;
};

} // namespace story_processors
} // namespace dcperf
