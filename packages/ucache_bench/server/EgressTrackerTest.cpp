// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include "cea/chips/benchpress/packages/ucache_bench/server/EgressTracker.h"

using facebook::ucachebench::EgressTracker;

TEST(EgressTrackerTest, StaysBoundedUnderDistinctKeys) {
  EgressTracker tracker;
  constexpr uint64_t kRequests = 1'000'000;

  for (uint64_t key = 0; key < kRequests; ++key) {
    tracker.record(key, 100);
    ASSERT_LE(
        tracker.keyEgress.size(),
        EgressTracker::kMaxKeysToTrack + EgressTracker::kCleanupInterval);
  }

  EXPECT_EQ(tracker.totalBytes, kRequests * 100);
}

TEST(EgressTrackerTest, TrimsToMaxKeysAtCleanup) {
  EgressTracker tracker;

  for (uint64_t key = 0; key < 4 * EgressTracker::kCleanupInterval; ++key) {
    tracker.record(key, 1);
  }

  EXPECT_EQ(tracker.cleanupCounter, 0);
  EXPECT_EQ(tracker.keyEgress.size(), EgressTracker::kMaxKeysToTrack);
}

TEST(EgressTrackerTest, AccumulatesRepeatedKey) {
  EgressTracker tracker;

  tracker.record(42, 100);
  tracker.record(42, 50);

  const std::pair<uint64_t, uint64_t> expected{150, 2};
  EXPECT_EQ(tracker.keyEgress.at(42), expected);
}
