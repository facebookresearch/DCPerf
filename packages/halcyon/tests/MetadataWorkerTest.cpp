// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "MetadataWorker.h"

#include "Common.h" // kMinIoSize, kChunkHeaderSize
#include "HalcyonChunkMapper.h"

#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

namespace fs = std::filesystem;

std::string uniqueDir(const std::string& tag) {
  return "/tmp/halcyon_mdworker_test_" + tag + "_" + std::to_string(::getpid());
}

// doDirectReads=false because /tmp is typically tmpfs, which rejects O_DIRECT.
HalcyonChunkMapper openMapper(const std::string& dir) {
  HalcyonChunkMapper::Options opts;
  opts.doDirectReads = false;
  return HalcyonChunkMapper(dir + "/chunkmap.rocksdb", opts);
}

} // namespace

// The worker drains requests, batches them into MultiGet, and posts each result
// back to the originating reactor's queue with the correct hit/miss + location.
TEST(MetadataWorkerTest, BatchedLookup_HitsAndMisses) {
  const std::string dir = uniqueDir("batch");
  fs::remove_all(dir);
  fs::create_directories(dir);

  constexpr ChunkId kNumChunks = 16;
  {
    HalcyonChunkMapper mapper = openMapper(dir);
    std::vector<std::pair<ChunkId, ChunkLocation>> entries;
    entries.reserve(kNumChunks);
    for (ChunkId id = 0; id < kNumChunks; ++id) {
      ChunkLocation loc;
      loc.fileIndex = static_cast<uint32_t>(id % 4);
      loc.length = kMinIoSize;
      loc.offset = kChunkHeaderSize + id * static_cast<uint64_t>(kMinIoSize);
      entries.emplace_back(id, loc);
    }
    mapper.putBatch(entries);
    mapper.setMaxChunkId(kNumChunks);
  }

  HalcyonChunkMapper mapper = openMapper(dir);
  LookupRequestQueue requestQueue(64);
  LookupResultQueue resultQueue(64);
  MetadataWorker worker(&requestQueue, {&resultQueue}, /*maxBatch=*/8);
  std::thread workerThread([&worker] { worker.run(); });

  // Even slots query an existing id (hit); odd slots query a far-out id (miss).
  // slotIndex doubles as the request tag.
  constexpr int kReqs = 20;
  for (int s = 0; s < kReqs; ++s) {
    LookupRequest req;
    req.mapper = &mapper;
    req.reactorId = 0;
    req.slotIndex = s;
    req.id = (s % 2 == 0) ? static_cast<ChunkId>(s % kNumChunks)
                          : kNumChunks + static_cast<ChunkId>(s); // miss
    requestQueue.blockingWrite(req);
  }

  // Collect all results, indexed by slotIndex (workers may post out of order).
  std::vector<LookupResult> got(kReqs);
  std::vector<bool> seen(kReqs, false);
  for (int i = 0; i < kReqs; ++i) {
    LookupResult res;
    // MayBlock=false queue has no reliable blocking read: spin (yielding) until
    // the worker posts the result, bounded so a bug fails the test rather than
    // hanging.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!resultQueue.try_dequeue(res)) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
          << "timed out waiting for result " << i;
      std::this_thread::yield();
    }
    ASSERT_GE(res.slotIndex, 0);
    ASSERT_LT(res.slotIndex, kReqs);
    got[res.slotIndex] = res;
    seen[res.slotIndex] = true;
  }
  worker.stop();
  workerThread.join();

  for (int s = 0; s < kReqs; ++s) {
    ASSERT_TRUE(seen[s]) << "missing result for slot " << s;
    if (s % 2 == 0) {
      const ChunkId id = static_cast<ChunkId>(s % kNumChunks);
      EXPECT_TRUE(got[s].found) << "slot " << s;
      EXPECT_EQ(
          got[s].loc.offset,
          kChunkHeaderSize + id * static_cast<uint64_t>(kMinIoSize));
      EXPECT_EQ(got[s].loc.length, static_cast<uint32_t>(kMinIoSize));
    } else {
      EXPECT_FALSE(got[s].found) << "slot " << s;
    }
  }

  fs::remove_all(dir);
}

} // namespace facebook::halcyon
