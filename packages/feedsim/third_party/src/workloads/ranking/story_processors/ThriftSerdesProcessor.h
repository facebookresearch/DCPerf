// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "StoryPrimitives.h"
#include "StoryProcessorBase.h"

namespace dcperf {
namespace story_processors {

// ThriftSerdesProcessor — mirrors prod
// `ViewStateStoryInfoSerializationUtils::populateStoryInfo` +
// `processStoryScoreInfos` + `signedIntToZigzag`. Per story: emit
// ~16 varint-encoded fields + one length-prefixed memcpy of a 64-256 B
// payload. Per-story CPU ~600 ns. Uses a thread-local buffer to avoid
// per-call allocation in the hot path.
class ThriftSerdesProcessor final : public StoryProcessorBase {
 public:
  void initialize(int complexity, uint64_t seed) override;
  void process(MockStoryList& stories, MockQueryCtx& ctx) override;
  std::string name() const override { return "thrift_serdes_pass"; }

 private:
  int complexity_ = 5;
};

} // namespace story_processors
} // namespace dcperf
