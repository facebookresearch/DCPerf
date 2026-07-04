// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Lightweight thread-safe latency histogram with log2 buckets, used to
// instrument hot paths (mock_services fanout, per-RPC dispatch, mock
// service-side simulated delay) for ad-hoc performance debugging without
// pulling in a full metrics framework.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace feedsim {

class LatencyHistogram {
 public:
  // log2 buckets: bucket b covers [2^(b-1), 2^b) us. Bucket 0 is the
  // "0 us" sample. 32 buckets reach 2^31 us ~ 35 minutes -- plenty.
  static constexpr int kNumBuckets = 32;

  void record(uint64_t us) {
    int b = (us == 0)
        ? 0
        : std::min<int>(kNumBuckets - 1, 64 - __builtin_clzll(us));
    buckets_[b].fetch_add(1, std::memory_order_relaxed);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    uint64_t cur = max_us_.load(std::memory_order_relaxed);
    while (us > cur &&
           !max_us_.compare_exchange_weak(
               cur, us, std::memory_order_relaxed)) {
    }
  }

  // Single-line summary with count, avg, p50/p95/p99 (bucket upper-bound),
  // max. Bucket bounds are powers of 2 -- intentionally coarse so the
  // dump remains compact.
  void dump(const std::string& label) const {
    uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0) {
      std::cerr << "[lh:" << label << "] count=0" << std::endl;
      return;
    }
    uint64_t sum = sum_us_.load(std::memory_order_relaxed);
    uint64_t mx = max_us_.load(std::memory_order_relaxed);
    uint64_t avg = sum / total;

    auto upper_us = [](int b) -> uint64_t {
      return b <= 0 ? 0ULL : (1ULL << b);
    };

    auto target = [total](double pct) -> uint64_t {
      return static_cast<uint64_t>(static_cast<double>(total) * pct);
    };
    uint64_t p50_t = target(0.50);
    uint64_t p95_t = target(0.95);
    uint64_t p99_t = target(0.99);

    int p50_b = -1, p95_b = -1, p99_b = -1;
    uint64_t cum = 0;
    for (int b = 0; b < kNumBuckets; ++b) {
      cum += buckets_[b].load(std::memory_order_relaxed);
      if (p50_b < 0 && cum >= p50_t) p50_b = b;
      if (p95_b < 0 && cum >= p95_t) p95_b = b;
      if (p99_b < 0 && cum >= p99_t) p99_b = b;
    }

    std::cerr << "[lh:" << label << "] count=" << total
              << " avg_us=" << avg << " p50_us<=" << upper_us(p50_b)
              << " p95_us<=" << upper_us(p95_b)
              << " p99_us<=" << upper_us(p99_b) << " max_us=" << mx
              << std::endl;
  }

 private:
  std::array<std::atomic<uint64_t>, kNumBuckets> buckets_{};
  std::atomic<uint64_t> sum_us_{0};
  std::atomic<uint64_t> count_{0};
  std::atomic<uint64_t> max_us_{0};
};

inline uint64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace feedsim
