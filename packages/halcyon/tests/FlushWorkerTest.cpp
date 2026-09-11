// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FlushWorker.h"

#include "Common.h" // kMinIoSize, kChunkHeaderSize
#include "HalcyonChunkMapper.h"

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

namespace fs = std::filesystem;

std::string uniqueDir(const std::string& tag) {
  return "/tmp/halcyon_flushworker_test_" + tag + "_" +
      std::to_string(::getpid());
}

// doDirectReads=false because /tmp is typically tmpfs, which rejects O_DIRECT.
HalcyonChunkMapper openMapper(const std::string& dir) {
  HalcyonChunkMapper::Options opts;
  opts.doDirectReads = false;
  return HalcyonChunkMapper(dir + "/chunkmap.rocksdb", opts);
}

ChunkLocation makeLoc(ChunkId id) {
  ChunkLocation loc;
  loc.fileIndex = static_cast<uint32_t>(id % 4);
  loc.length = kMinIoSize;
  loc.offset = kChunkHeaderSize + id * static_cast<uint64_t>(kMinIoSize);
  return loc;
}

} // namespace

// The worker drains flush requests, batches them per mapper into putBatch +
// syncWal, persists every mapping, and decrements each request's pending
// counter (the per-reactor handle a teardown drain waits on). Requests are
// spread across two mappers to exercise the group-by-mapper path.
TEST(FlushWorkerTest, BatchedFlush_PersistsAndSignalsPending) {
  const std::string dirA = uniqueDir("a");
  const std::string dirB = uniqueDir("b");
  for (const auto& dir : {dirA, dirB}) {
    fs::remove_all(dir);
    fs::create_directories(dir);
  }

  constexpr ChunkId kNumChunks = 40;

  // Originals live in an inner scope so they (and their RocksDB locks) are torn
  // down before the verification reopen. The worker holds the mapper pointers,
  // so it must be stopped + joined before the mappers are destroyed.
  {
    HalcyonChunkMapper mapperA = openMapper(dirA);
    HalcyonChunkMapper mapperB = openMapper(dirB);

    // One pending counter per mapper, mirroring the per-reactor pendingFlush_.
    std::atomic<int> pendingA{0};
    std::atomic<int> pendingB{0};

    FlushRequestQueue requestQueue(256);
    FlushWorker worker(&requestQueue, /*maxBatch=*/8);
    std::thread workerThread([&worker] { worker.run(); });

    // Even ids -> mapper A, odd ids -> mapper B. The reactor increments its
    // pending counter before enqueuing each flush.
    for (ChunkId id = 0; id < kNumChunks; ++id) {
      FlushRequest req;
      if (id % 2 == 0) {
        req.mapper = &mapperA;
        req.pending = &pendingA;
        pendingA.fetch_add(1, std::memory_order_relaxed);
      } else {
        req.mapper = &mapperB;
        req.pending = &pendingB;
        pendingB.fetch_add(1, std::memory_order_relaxed);
      }
      req.id = id;
      req.loc = makeLoc(id);
      requestQueue.blockingWrite(req);
    }

    // Wait for the worker to drain everything (both counters back to zero),
    // bounded so a bug fails the test rather than hanging.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pendingA.load(std::memory_order_acquire) != 0 ||
           pendingB.load(std::memory_order_acquire) != 0) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
          << "timed out waiting for flushes; pendingA="
          << pendingA.load(std::memory_order_acquire)
          << " pendingB=" << pendingB.load(std::memory_order_acquire);
      std::this_thread::yield();
    }

    worker.stop();
    workerThread.join();
  }

  // Reopen each mapper and confirm every mapping landed durably.
  HalcyonChunkMapper reopenedA = openMapper(dirA);
  HalcyonChunkMapper reopenedB = openMapper(dirB);
  for (ChunkId id = 0; id < kNumChunks; ++id) {
    HalcyonChunkMapper& m = (id % 2 == 0) ? reopenedA : reopenedB;
    ChunkLocation loc;
    EXPECT_TRUE(m.lookup(id, loc)) << "missing mapping for id " << id;
    const ChunkLocation want = makeLoc(id);
    EXPECT_EQ(loc.fileIndex, want.fileIndex) << "id " << id;
    EXPECT_EQ(loc.length, want.length) << "id " << id;
    EXPECT_EQ(loc.offset, want.offset) << "id " << id;
  }

  for (const auto& dir : {dirA, dirB}) {
    fs::remove_all(dir);
  }
}

} // namespace facebook::halcyon
