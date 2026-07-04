// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "TopKProcessor.h"

#include <algorithm>

namespace dcperf {
namespace story_processors {

void TopKProcessor::initialize(int complexity, uint64_t seed) {
  complexity_ = complexity;
  (void)seed;
}

void TopKProcessor::process(MockStoryList& stories, MockQueryCtx& ctx) {
  if (stories.empty()) {
    return;
  }
  const size_t k = (ctx.topk == 0) ? 50u : static_cast<size_t>(ctx.topk);
  const size_t effective_k = std::min(k, stories.size());
  std::partial_sort(
      stories.begin(),
      stories.begin() + effective_k,
      stories.end(),
      [](const MockStory& a, const MockStory& b) {
        return a.score > b.score;
      });
  // Trim to top-K — matches prod's "drop everything beyond pass1 topK".
  stories.resize(effective_k);
}

} // namespace story_processors
} // namespace dcperf
