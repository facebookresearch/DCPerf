// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FilteringPassProcessor.h"

namespace dcperf {
namespace story_processors {

void FilteringPassProcessor::initialize(int complexity, uint64_t seed) {
  complexity_ = complexity;
  filter_seed_ = seed;
}

void FilteringPassProcessor::process(
    MockStoryList& stories, MockQueryCtx& ctx) {
  // Linear vector scan with a small predicate set. The scoring pass
  // tagged candidates with bit 0x100; filter sweeps them out. The min
  // weight gate mirrors prod's "weight under threshold → drop" decision
  // in BadObjectFilterUtils::filterBadObjects.
  const float min_weight = 0.05f;
  const uint32_t drop_mask = 0x100u;
  auto write_it = stories.begin();
  for (auto read_it = stories.begin(); read_it != stories.end(); ++read_it) {
    if ((read_it->flags & drop_mask) != 0u) {
      continue;
    }
    if (read_it->weight < min_weight) {
      continue;
    }
    if (read_it->story_type < 0) {
      continue;
    }
    if (write_it != read_it) {
      *write_it = std::move(*read_it);
    }
    ++write_it;
  }
  stories.erase(write_it, stories.end());
  (void)ctx;
}

} // namespace story_processors
} // namespace dcperf
