// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <vector>

namespace dcperf {
namespace story_processors {

// Mock story score-info: mirrors prod multifeed
// `ranking::ViewStateStoryInfo` and `StoryScoreInfo` (~64 bytes), the
// objects scored & ranked through the pipeline passes. Field set is
// calibrated to land per-story serialization work in prod's ~5% band.
struct MockStoryScoreInfo {
  int64_t story_key;
  int64_t actor_id;
  int64_t target_id;
  int32_t source_type;
  int32_t story_type;
  int32_t time_published;
  float weight;
  float weight_user;
  float weight_event;
  float discounted_weight;
  uint32_t flags;
  uint32_t padding;
};

// Mock story: ~256 bytes, sized to land in the L2-resident working set
// across a pipeline of 50–400 stories (matches prod aggregator's
// `FeedStoryList` per-request slice).
struct MockStory {
  int64_t story_id;
  int32_t story_type;
  int32_t source_type;
  float score;
  float weight;
  uint32_t flags;
  uint32_t reserved;
  std::vector<int64_t> score_keys; // ~8 entries typical
  std::vector<float> raw_scores; // matches score_keys.size()
  MockStoryScoreInfo score_info;
  // Padding to land in 256-byte cache lines (vector overhead aside).
  char extra[64];
};

// Per-request query context shared across pipeline stages.
struct MockQueryCtx {
  int64_t query_id;
  int64_t user_id;
  uint64_t seed;
  uint32_t topk;
  uint32_t min_gap_window;
};

using MockStoryList = std::vector<MockStory>;

// Buffer holding serialized score info, written by ThriftSerdesProcessor.
// Reused across pipeline passes within a single thread.
using SerdesBuffer = std::vector<uint8_t>;

} // namespace story_processors
} // namespace dcperf
