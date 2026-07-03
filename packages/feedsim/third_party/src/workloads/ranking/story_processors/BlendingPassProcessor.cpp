// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "BlendingPassProcessor.h"

#include <algorithm>
#include <random>

namespace dcperf {
namespace story_processors {

void BlendingPassProcessor::initialize(int complexity, uint64_t seed) {
  complexity_ = complexity;
  // Pre-build a fixed auxiliary list of K stories (sorted by score
  // descending) that gets merged into `stories` on every process()
  // call. K = 8 * complexity gives ~40 entries at default complexity
  // 5, matching prod's typical promoted-stories blend size.
  const size_t K = 8 * static_cast<size_t>(complexity);
  aux_list_.resize(K);
  std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ull);
  std::uniform_real_distribution<float> score_dist(0.1f, 1.0f);
  std::uniform_int_distribution<int32_t> stype_dist(0, 25);
  for (size_t i = 0; i < K; ++i) {
    aux_list_[i].story_id = static_cast<int64_t>(i ^ seed);
    aux_list_[i].story_type = stype_dist(rng);
    aux_list_[i].source_type = stype_dist(rng) % 8;
    aux_list_[i].score = score_dist(rng);
    aux_list_[i].weight = aux_list_[i].score;
    aux_list_[i].flags = 0x1u;
    aux_list_[i].score_keys.assign({0, 1, 2, 3});
    aux_list_[i].raw_scores.assign({0.1f, 0.2f, 0.3f, 0.4f});
  }
  std::sort(aux_list_.begin(), aux_list_.end(),
            [](const MockStory& a, const MockStory& b) {
              return a.score > b.score;
            });
}

void BlendingPassProcessor::process(MockStoryList& stories, MockQueryCtx& ctx) {
  if (stories.empty()) {
    return;
  }
  // Sort input by score desc to make the merge meaningful (cheap on
  // small lists post-filter). Prod's blender input is already
  // pre-sorted by pass1 ranking, so this mostly tests std::sort's
  // best-case branch behavior.
  std::sort(stories.begin(), stories.end(),
            [](const MockStory& a, const MockStory& b) {
              return a.score > b.score;
            });

  // 2-way merge with min-gap state machine: track the last K source
  // types and never emit a duplicate within `ctx.min_gap_window`
  // positions. This matches MinGapRuleEnforcementComponent::enforce
  // — small per-emit branch + ring-buffer mod arithmetic.
  MockStoryList out;
  out.reserve(stories.size() + aux_list_.size());

  const uint32_t window = ctx.min_gap_window ? ctx.min_gap_window : 4u;
  std::vector<int32_t> recent(window, -1);
  uint32_t pos = 0;

  auto try_emit = [&](MockStory&& s) {
    // Check min-gap: source_type must not appear in `recent` window.
    // On violation, bump pos so the ring overwrites the violating slot
    // first on the next match. This keeps the work shape similar to
    // prod (still emit — no defer-queue in the mock) but the accounting
    // reflects the extra slot advance.
    for (uint32_t i = 0; i < window; ++i) {
      if (recent[i] == s.source_type) {
        ++pos;
        break;
      }
    }
    recent[pos % window] = s.source_type;
    ++pos;
    out.push_back(std::move(s));
  };

  size_t a = 0, b = 0;
  while (a < stories.size() && b < aux_list_.size()) {
    if (stories[a].score >= aux_list_[b].score) {
      try_emit(std::move(stories[a]));
      ++a;
    } else {
      // aux_list_ is read-only across requests; copy not move.
      try_emit(MockStory(aux_list_[b]));
      ++b;
    }
  }
  while (a < stories.size()) {
    try_emit(std::move(stories[a]));
    ++a;
  }
  while (b < aux_list_.size()) {
    try_emit(MockStory(aux_list_[b]));
    ++b;
  }
  stories.swap(out);
}

} // namespace story_processors
} // namespace dcperf
