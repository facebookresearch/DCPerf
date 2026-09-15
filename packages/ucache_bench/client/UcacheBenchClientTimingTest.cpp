/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "cea/chips/benchpress/packages/ucache_bench/client/UcacheBenchClient.h"

namespace facebook::ucachebench {

TEST(
    UcacheBenchMeasurementWindowTest,
    DistributesProcessStartsDeterministically) {
  EXPECT_EQ(firstDispatchTimeoutSeconds(0), 30);
  EXPECT_EQ(firstDispatchTimeoutSeconds(16), 46);
  EXPECT_EQ(immediateFailureWindow(123).start, 123);
  EXPECT_EQ(immediateFailureWindow(123).end, 123);
  EXPECT_TRUE(countWindowEndCancellation(true));
  EXPECT_FALSE(countWindowEndCancellation(false));
  EXPECT_EQ(processRampDelayNs(1, 32, 16), 0);
  EXPECT_EQ(processRampDelayNs(2, 32, 16), 500'000'000);
  EXPECT_EQ(processRampDelayNs(32, 32, 16), 15'500'000'000);
}

TEST(UcacheBenchMeasurementWindowTest, CountsOnlyMeasuredSkippedSequences) {
  EXPECT_EQ(countSequenceIntersection(80, 120, 100, 200), 20);
  EXPECT_EQ(countSequenceIntersection(120, 180, 100, 200), 60);
  EXPECT_EQ(countSequenceIntersection(180, 220, 100, 200), 20);
  EXPECT_EQ(countSequenceIntersection(0, 80, 100, 200), 0);
}

TEST(UcacheBenchMeasurementWindowTest, AlignsOnlySafeFutureWallTimes) {
  EXPECT_EQ(alignWallTimeToSteadyClock(12'000, 10'000, 4'000), 6'000);
  EXPECT_EQ(alignWallTimeToSteadyClock(10'000, 10'000, 4'000), std::nullopt);
  EXPECT_EQ(alignWallTimeToSteadyClock(9'000, 10'000, 4'000), std::nullopt);
  EXPECT_EQ(
      alignWallTimeToSteadyClock(
          std::numeric_limits<int64_t>::max(),
          0,
          std::numeric_limits<int64_t>::max()),
      std::nullopt);
  EXPECT_EQ(checkedMeasurementEndNs(10'000, 240), 240'000'010'000);
  EXPECT_EQ(
      checkedMeasurementEndNs(std::numeric_limits<int64_t>::max(), 1),
      std::nullopt);
}

TEST(
    UcacheBenchMeasurementWindowTest,
    RefillBehaviorDoesNotDependOnAccounting) {
  EXPECT_TRUE(shouldRefillGetMiss(false, false));
  EXPECT_TRUE(shouldRefillGetMiss(false, true));
  EXPECT_TRUE(shouldRefillGetMiss(true, true));
  EXPECT_FALSE(shouldRefillGetMiss(true, false));
}

TEST(UcacheBenchMeasurementWindowTest, IncludesStartAndExcludesEnd) {
  EXPECT_FALSE(isInMeasurementWindow(99, 100, 340));
  EXPECT_TRUE(isInMeasurementWindow(100, 100, 340));
  EXPECT_TRUE(isInMeasurementWindow(339, 100, 340));
  EXPECT_FALSE(isInMeasurementWindow(340, 100, 340));
}

TEST(UcacheBenchMeasurementWindowTest, HasExactRequestedDuration) {
  constexpr int64_t kStartNs = 7'000'000'000;
  constexpr int64_t kDurationNs = 240'000'000'000;
  constexpr int64_t kEndNs = kStartNs + kDurationNs;

  EXPECT_TRUE(isInMeasurementWindow(kStartNs, kStartNs, kEndNs));
  EXPECT_TRUE(isInMeasurementWindow(kEndNs - 1, kStartNs, kEndNs));
  EXPECT_FALSE(isInMeasurementWindow(kEndNs, kStartNs, kEndNs));
}

} // namespace facebook::ucachebench
