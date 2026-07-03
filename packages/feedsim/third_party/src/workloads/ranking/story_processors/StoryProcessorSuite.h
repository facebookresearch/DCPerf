// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>
#include <memory>
#include <random>

#include "BlendingPassProcessor.h"
#include "FilteringPassProcessor.h"
#include "ScoringPassProcessor.h"
#include "StoryPrimitives.h"
#include "ThriftSerdesProcessor.h"
#include "TopKProcessor.h"

namespace dcperf {
namespace story_processors {

// StoryProcessorSuite — orchestrates a full pipeline pass over a
// MockStoryList: scoring → filtering → blending → topK → serdes.
//
// **Concurrency contract:** LeafNodeRank dispatches `runStoryProcessing`
// onto `cpuThreadPool` (folly::getGlobalCPUExecutor — multi-threaded), and
// multiple in-flight requests can call into the SAME ThreadData's
// `story_suite` concurrently. The suite must therefore be thread-safe in
// `runPipelinePasses`. The mutable working buffer is thread_local inside
// `runOnePipelinePass` (each pool worker gets its own); the read-only
// `template_list_` and per-archetype hash tables are mutated only in
// `initialize()` and never after, so concurrent reads are safe.
// The previously-member `working_` and `query_id_counter_` were the
// source of a SIGSEGV crash analogous to the t27 feature-extractor
// concurrent-mutation bug (D106731815). See
// `[[t29-crash-fix-validation-results]]` memory for context.
class StoryProcessorSuite {
 public:
  StoryProcessorSuite() = default;

  // Build a pre-populated MockStoryList of `count` stories that the
  // pipeline operates on. Lives as a member so we don't reallocate
  // every request. The template list is read-only after init; each
  // pipeline pass copies it into a thread_local working buffer.
  void initialize(int complexity, uint64_t seed, size_t stories_per_pass);

  // Run one full pipeline pass. Thread-safe — see class-level comment.
  void runOnePipelinePass();

  // Run N pipeline passes back-to-back. Thread-safe.
  void runPipelinePasses(int n);

  size_t storiesPerPass() const { return stories_per_pass_; }

 private:
  std::unique_ptr<ScoringPassProcessor> scoring_;
  std::unique_ptr<FilteringPassProcessor> filtering_;
  std::unique_ptr<BlendingPassProcessor> blending_;
  std::unique_ptr<TopKProcessor> topk_;
  std::unique_ptr<ThriftSerdesProcessor> serdes_;

  // Read-only after initialize(). Concurrent reads from multiple
  // GlobalCPUThread workers are safe.
  MockStoryList template_list_;

  // Atomic counter so concurrent pipeline passes get distinct
  // query_ids without racing.
  std::atomic<uint64_t> query_id_counter_{0};
  uint64_t seed_ = 0;
  size_t stories_per_pass_ = 0;
};

} // namespace story_processors
} // namespace dcperf
