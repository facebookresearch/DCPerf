/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <folly/coro/AsyncScope.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/CurrentExecutor.h>
#include <folly/coro/DetachOnCancel.h>
#include <folly/coro/Task.h>
#include <folly/futures/Promise.h>

#include "cea/chips/benchpress/packages/ucache_bench/client/UcacheBenchClient.h"

namespace facebook::ucachebench {
namespace {

folly::coro::Task<void> awaitDetachedFuture(
    folly::SemiFuture<int> future,
    folly::coro::Baton& started,
    std::atomic<bool>& cancelled) {
  started.post();
  try {
    (void)co_await folly::coro::detachOnCancel(std::move(future));
  } catch (const folly::OperationCancelled&) {
    cancelled.store(true, std::memory_order_relaxed);
  }
  co_return;
}

} // namespace

TEST(
    UcacheBenchMeasurementWindowTest,
    DistributesProcessStartsDeterministically) {
  EXPECT_EQ(rampCoordinationTimeoutSeconds(0), 180);
  EXPECT_EQ(rampCoordinationTimeoutSeconds(16), 196);
  EXPECT_EQ(rampCoordinationTimeoutSeconds(64), 244);
  EXPECT_EQ(immediateFailureWindow(123).start, 123);
  EXPECT_EQ(immediateFailureWindow(123).end, 123);
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

TEST(UcacheBenchLatencySamplingTest, IsolatesWorkerStateByCacheLine) {
  EXPECT_GE(alignof(WorkerLatencySampler), 64);
}

TEST(UcacheBenchLatencySamplingTest, KeepsLateLowPrioritySamples) {
  std::vector<PrioritizedLatencySample> samples;
  recordPrioritizedLatencySample(samples, 10, 1.0, 2);
  recordPrioritizedLatencySample(samples, 20, 2.0, 2);
  recordPrioritizedLatencySample(samples, 5, 3.0, 2);
  std::sort(samples.begin(), samples.end());

  const std::vector<PrioritizedLatencySample> expected{{5, 3.0}, {10, 1.0}};
  EXPECT_EQ(samples, expected);
}

TEST(UcacheBenchLatencySamplingTest, UsesWorkerSpecificPriorities) {
  EXPECT_NE(latencySamplePriority(256, 0, 1), latencySamplePriority(256, 1, 1));
  EXPECT_NE(latencySamplePriority(256, 0, 1), latencySamplePriority(512, 0, 1));
  EXPECT_EQ(latencySamplePriority(256, 0, 1), latencySamplePriority(256, 0, 1));
}

TEST(UcacheBenchOutstandingTest, ExplicitReleaseReopensCapWithOwnersAlive) {
  auto tracker = std::make_shared<detail::PhysicalOutstandingTracker>();
  auto senderOwner = tracker->tryAcquire(1);
  ASSERT_NE(senderOwner, nullptr);
  EXPECT_EQ(tracker->outstanding(), 1);
  EXPECT_EQ(tracker->tryAcquire(1), nullptr);

  auto callbackOwner = senderOwner;
  callbackOwner->release();
  EXPECT_EQ(tracker->outstanding(), 0);

  auto nextOwner = tracker->tryAcquire(1);
  ASSERT_NE(nextOwner, nullptr);
  EXPECT_EQ(tracker->outstanding(), 1);

  callbackOwner->release();
  EXPECT_EQ(tracker->outstanding(), 1);
  senderOwner.reset();
  callbackOwner.reset();
  EXPECT_EQ(tracker->outstanding(), 1);

  nextOwner->release();
  EXPECT_EQ(tracker->outstanding(), 0);
}

TEST(UcacheBenchOutstandingTest, DestructorReleasesLease) {
  auto tracker = std::make_shared<detail::PhysicalOutstandingTracker>();
  {
    auto lease = tracker->tryAcquire(1);
    ASSERT_NE(lease, nullptr);
    EXPECT_EQ(tracker->outstanding(), 1);
  }
  EXPECT_EQ(tracker->outstanding(), 0);
}

TEST(UcacheBenchOutstandingTest, CancellationDetachesBeforePhysicalCompletion) {
  auto tracker = std::make_shared<detail::PhysicalOutstandingTracker>();
  auto physicalLease = tracker->tryAcquire(1);
  ASSERT_NE(physicalLease, nullptr);
  auto [promise, future] = folly::makePromiseContract<int>();
  std::atomic<bool> cancelled{false};
  std::atomic<bool> callbackRan{false};
  auto callback =
      [p = std::move(promise), physicalLease, &callbackRan](int value) mutable {
        callbackRan.store(true, std::memory_order_relaxed);
        p.setValue(value);
        physicalLease->release();
      };

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    folly::coro::CancellableAsyncScope scope;
    folly::coro::Baton started;
    auto executor = co_await folly::coro::co_current_executor;
    scope.add(
        folly::coro::co_withExecutor(
            executor,
            awaitDetachedFuture(std::move(future), started, cancelled)));

    co_await started;
    co_await scope.cancelAndJoinAsync();
    EXPECT_TRUE(cancelled.load(std::memory_order_relaxed));
    EXPECT_FALSE(callbackRan.load(std::memory_order_relaxed));
    EXPECT_EQ(tracker->outstanding(), 1);
    EXPECT_EQ(tracker->tryAcquire(1), nullptr);

    callback(7);
    EXPECT_TRUE(callbackRan.load(std::memory_order_relaxed));
    EXPECT_EQ(tracker->outstanding(), 0);
  }());
}

TEST(
    UcacheBenchMeasurementWindowTest,
    RefillBehaviorDoesNotDependOnAccounting) {
  EXPECT_TRUE(shouldUsePerRequestTimeout(false));
  EXPECT_FALSE(shouldUsePerRequestTimeout(true));
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
