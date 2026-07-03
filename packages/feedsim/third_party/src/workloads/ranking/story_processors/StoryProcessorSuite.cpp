// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "StoryProcessorSuite.h"

#include <random>

namespace dcperf {
namespace story_processors {

void StoryProcessorSuite::initialize(
    int complexity, uint64_t seed, size_t stories_per_pass) {
  seed_ = seed;
  stories_per_pass_ = stories_per_pass;

  scoring_ = std::make_unique<ScoringPassProcessor>();
  filtering_ = std::make_unique<FilteringPassProcessor>();
  blending_ = std::make_unique<BlendingPassProcessor>();
  topk_ = std::make_unique<TopKProcessor>();
  serdes_ = std::make_unique<ThriftSerdesProcessor>();

  // Per-archetype seed = base seed XOR'd with archetype-specific
  // constant to avoid all four hash tables sharing the same
  // collision pattern.
  scoring_->initialize(complexity, seed_ ^ 0xa1b2c3d4e5f60718ull);
  filtering_->initialize(complexity, seed_ ^ 0xbf58476d1ce4e5b9ull);
  blending_->initialize(complexity, seed_ ^ 0x94d049bb133111ebull);
  topk_->initialize(complexity, seed_);
  serdes_->initialize(complexity, seed_);

  // Build the template list once. Each story has a small set of
  // score_keys and raw_scores so the scoring inner loop has predictable
  // (yet diverse) F14 lookup patterns.
  template_list_.resize(stories_per_pass_);
  std::mt19937_64 rng(seed_ ^ 0x243f6a8885a308d3ull);
  std::uniform_int_distribution<int64_t> key_dist(0, (1LL << 24) - 1);
  std::uniform_real_distribution<float> score_dist(0.0f, 1.0f);
  std::uniform_int_distribution<int32_t> stype_dist(0, 25);
  for (size_t i = 0; i < stories_per_pass_; ++i) {
    auto& s = template_list_[i];
    s.story_id = static_cast<int64_t>(i);
    s.story_type = stype_dist(rng);
    s.source_type = stype_dist(rng) % 8;
    s.score = score_dist(rng);
    s.weight = score_dist(rng);
    s.flags = 0x1u | ((rng() & 0xfu) << 4u);
    s.score_keys.resize(8);
    s.raw_scores.resize(8);
    for (size_t j = 0; j < 8; ++j) {
      s.score_keys[j] = key_dist(rng);
      s.raw_scores[j] = score_dist(rng);
    }
    s.score_info.story_key = s.story_id;
    s.score_info.actor_id = key_dist(rng);
    s.score_info.target_id = key_dist(rng);
    s.score_info.source_type = s.source_type;
    s.score_info.story_type = s.story_type;
    s.score_info.time_published = static_cast<int32_t>(rng() & 0x7fffffff);
    s.score_info.weight = s.weight;
    s.score_info.weight_user = 0.0f;
    s.score_info.weight_event = score_dist(rng);
    s.score_info.discounted_weight = 0.0f;
    s.score_info.flags = s.flags;
  }
}

void StoryProcessorSuite::runOnePipelinePass() {
  // Per-thread working buffer. Critical for correctness: this suite is
  // dispatched onto GlobalCPUThread (folly's multi-threaded CPU pool)
  // so multiple workers can call runPipelinePasses() on the same suite
  // concurrently. A shared `working_` member would race on the
  // std::vector internal pointer during resize/swap → SIGSEGV under
  // load (matches the t27 feature-extractor crash pattern fixed by
  // D106731815). thread_local makes each worker thread carry its own
  // buffer; capacity persists across calls so no per-pass realloc.
  thread_local MockStoryList t_working;
  t_working = template_list_;

  MockQueryCtx ctx;
  const uint64_t qid =
      query_id_counter_.fetch_add(1, std::memory_order_relaxed) + 1;
  ctx.query_id = static_cast<int64_t>(qid);
  ctx.user_id = static_cast<int64_t>(seed_ ^ qid);
  ctx.seed = seed_ ^ qid;
  ctx.topk = 50;
  ctx.min_gap_window = 4;

  // Pipeline order matches prod multifeed:
  //   ValueModelApplier::calculateStoryScores → applyFilteringBeforePass0
  //   → doRegularPass1Ranking (topK) → BlenderPass::blendStories
  //   → ViewStateStoryInfoSerializationUtils::populateStoryInfo
  scoring_->process(t_working, ctx);
  filtering_->process(t_working, ctx);
  topk_->process(t_working, ctx);
  blending_->process(t_working, ctx);
  serdes_->process(t_working, ctx);
}

void StoryProcessorSuite::runPipelinePasses(int n) {
  for (int i = 0; i < n; ++i) {
    runOnePipelinePass();
  }
}

} // namespace story_processors
} // namespace dcperf
