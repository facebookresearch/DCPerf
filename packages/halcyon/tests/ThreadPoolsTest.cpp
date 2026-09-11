// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ThreadPools.h"

#include <pthread.h>
#include <sched.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <folly/futures/Future.h>
#include <folly/system/ThreadName.h>

#include <gtest/gtest.h>

namespace facebook::halcyon {

namespace {

// Busy-spins on the calling thread for at least the given duration so the
// thread accrues measurable CPU time (as opposed to sleeping, which does not).
void burnCpuFor(std::chrono::milliseconds duration) {
  volatile uint64_t sink = 0;
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    for (int i = 0; i < 10000; ++i) {
      sink += i;
    }
  }
}

// Returns the lowest CPU core this process is allowed to run on, so pinning
// tests use a core that is guaranteed available in the current cgroup.
int lowestAllowedCore() {
  cpu_set_t cpuSet;
  CPU_ZERO(&cpuSet);
  if (sched_getaffinity(0, sizeof(cpuSet), &cpuSet) != 0) {
    return 0;
  }
  for (int core = 0; core < CPU_SETSIZE; ++core) {
    if (CPU_ISSET(core, &cpuSet)) {
      return core;
    }
  }
  return 0;
}

} // namespace

TEST(ThreadPoolsTest, BuildPools_DefaultConfig_NamesThreadsByRole) {
  HalcyonPools pools = buildPools(PoolConfig{.qioThreads = 2});

  const std::string qioName =
      folly::via(pools.qioPool.get(), [] {
        return folly::getCurrentThreadName().value_or("");
      }).get();

  EXPECT_EQ(qioName.rfind("QIOThread", 0), 0u) << "got: " << qioName;
}

TEST(ThreadPoolsTest, GetCpuNs_AfterCpuWork_Accumulates) {
  HalcyonPools pools = buildPools(PoolConfig{.qioThreads = 1});

  folly::via(pools.qioPool.get(), [] {
    burnCpuFor(std::chrono::milliseconds(50));
  }).get();

  EXPECT_GT(pools.getCpuNs(PoolRole::QIOThread), 0u);
}

TEST(ThreadPoolsTest, BuildPools_PinningEnabled_RestrictsThreadAffinity) {
  const int core = lowestAllowedCore();
  HalcyonPools pools = buildPools(
      PoolConfig{.qioThreads = 1, .pinEnabled = true, .qioCores = {core}});

  const int pinnedCount =
      folly::via(pools.qioPool.get(), [] {
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        pthread_getaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet);
        return CPU_COUNT(&cpuSet);
      }).get();

  EXPECT_EQ(pinnedCount, 1);
}

TEST(ThreadPoolsTest, MakeReactorThread_NamesAndAccountsRawThread) {
  HalcyonPools pools = buildPools(PoolConfig{.qioThreads = 1});

  std::string name;
  std::thread thread = pools.makeReactorThread([&name] {
    name = folly::getCurrentThreadName().value_or("");
    burnCpuFor(std::chrono::milliseconds(50));
  });
  thread.join();

  EXPECT_EQ(name.rfind("Reactor", 0), 0u) << "got: " << name;
  EXPECT_GT(pools.getCpuNs(PoolRole::Reactor), 0u);
}

// The QMetadata lookup-worker pool is a distinct, role-named, CPU-accounted
// pool (the lookup workers run as tasks on it), separate from QIOThread.
TEST(ThreadPoolsTest, BuildPools_QMetadataPool_NamesAndAccountsCpu) {
  HalcyonPools pools =
      buildPools(PoolConfig{.qioThreads = 1, .qmetaThreads = 2});

  EXPECT_EQ(&pools.pool(PoolRole::QMetadata), pools.qmetaPool.get());

  const std::string name = folly::via(pools.qmetaPool.get(), [] {
                             burnCpuFor(std::chrono::milliseconds(50));
                             return folly::getCurrentThreadName().value_or("");
                           }).get();

  EXPECT_EQ(name.rfind("QMetadata", 0), 0u) << "got: " << name;
  EXPECT_GT(pools.getCpuNs(PoolRole::QMetadata), 0u);
  // QIOThread did no work, so its accounting stays separate (zero).
  EXPECT_EQ(pools.getCpuNs(PoolRole::QIOThread), 0u);
}

// The QNet send-worker pool is a distinct, role-named, CPU-accounted pool (the
// synthetic-partner network send workers run as tasks on it), separate from the
// QIOThread and QMetadata pools.
TEST(ThreadPoolsTest, BuildPools_QNetPool_NamesAndAccountsCpu) {
  HalcyonPools pools =
      buildPools(PoolConfig{.qioThreads = 1, .qnetThreads = 2});

  EXPECT_EQ(&pools.pool(PoolRole::QNet), pools.qnetPool.get());

  const std::string name = folly::via(pools.qnetPool.get(), [] {
                             burnCpuFor(std::chrono::milliseconds(50));
                             return folly::getCurrentThreadName().value_or("");
                           }).get();

  EXPECT_EQ(name.rfind("QNet", 0), 0u) << "got: " << name;
  EXPECT_GT(pools.getCpuNs(PoolRole::QNet), 0u);
  // QIOThread did no work, so its accounting stays separate (zero).
  EXPECT_EQ(pools.getCpuNs(PoolRole::QIOThread), 0u);
}

// The QFlush chunk-map write-offload pool is a distinct, role-named,
// CPU-accounted pool (flush workers run as tasks on it), separate from
// QIOThread.
TEST(ThreadPoolsTest, BuildPools_QFlushPool_NamesAndAccountsCpu) {
  HalcyonPools pools =
      buildPools(PoolConfig{.qioThreads = 1, .qflushThreads = 2});

  EXPECT_EQ(&pools.pool(PoolRole::QFlush), pools.qflushPool.get());

  const std::string name = folly::via(pools.qflushPool.get(), [] {
                             burnCpuFor(std::chrono::milliseconds(50));
                             return folly::getCurrentThreadName().value_or("");
                           }).get();

  EXPECT_EQ(name.rfind("QFlush", 0), 0u) << "got: " << name;
  EXPECT_GT(pools.getCpuNs(PoolRole::QFlush), 0u);
  // QIOThread did no work, so its accounting stays separate (zero).
  EXPECT_EQ(pools.getCpuNs(PoolRole::QIOThread), 0u);
}

} // namespace facebook::halcyon
