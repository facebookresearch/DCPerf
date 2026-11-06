/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/BenchmarkUtil.h>
#include <folly/container/F14Map.h>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace facebook::ucachebench {

// Per-thread stand-in for production NetworkOverloadProtector, which tracks
// per-key egress in a ConcurrentLRUHashMap bounded by maxKeysToTrack.
struct EgressTracker {
  static constexpr size_t kMaxKeysToTrack = 4096;
  static constexpr uint32_t kCleanupInterval = 8192;

  folly::F14FastMap<uint64_t, std::pair<uint64_t, uint64_t>> keyEgress;
  uint64_t totalBytes{0};
  uint32_t cleanupCounter{0};

  void record(uint64_t keyHash, size_t responseSize) {
    auto it = keyEgress.find(keyHash);
    if (it == keyEgress.end()) {
      keyEgress[keyHash] = {responseSize, 1};
    } else {
      // Sliding window read, token bucket check, sliding window write.
      auto& [bytes, count] = it->second;
      uint64_t windowValue = bytes;
      folly::doNotOptimizeAway(windowValue);
      bool hasTokens = (count < 10000);
      folly::doNotOptimizeAway(hasTokens);
      bytes += responseSize;
      count++;
    }

    totalBytes += responseSize;

    if (++cleanupCounter >= kCleanupInterval) {
      cleanupCounter = 0;
      while (keyEgress.size() > kMaxKeysToTrack) {
        keyEgress.erase(keyEgress.begin());
      }
    }
  }
};

} // namespace facebook::ucachebench
