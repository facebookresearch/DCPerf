// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FlushWorker.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include <folly/container/F14Map.h>
#include <folly/portability/Asm.h>

namespace facebook::halcyon {

namespace {

// Reads up to maxBatch requests (non-blocking) into `batch` (cleared first);
// returns the number drained.
size_t drainInto(
    FlushRequestQueue& q,
    size_t maxBatch,
    std::vector<FlushRequest>& batch) {
  batch.clear();
  FlushRequest req;
  while (batch.size() < maxBatch && q.read(req)) {
    batch.push_back(req);
  }
  return batch.size();
}

} // namespace

FlushWorker::FlushWorker(
    FlushRequestQueue* requestQueue,
    size_t maxBatch,
    bool busyPoll)
    : requestQueue_(requestQueue),
      maxBatch_(std::max<size_t>(1, maxBatch)),
      busyPoll_(busyPoll) {}

void FlushWorker::stop() {
  stop_.store(true, std::memory_order_release);
}

void FlushWorker::processBatch(std::vector<FlushRequest>& batch) {
  // Group by mapper so each putBatch + syncWal hits a single RocksDB instance
  // (mirrors HyperNode batching write-back per partition).
  folly::F14FastMap<HalcyonChunkMapper*, std::vector<size_t>> byMapper;
  for (size_t i = 0; i < batch.size(); ++i) {
    byMapper[batch.at(i).mapper].push_back(i);
  }

  std::vector<std::pair<ChunkId, ChunkLocation>> entries;
  for (auto& [mapper, idxs] : byMapper) {
    entries.clear();
    entries.reserve(idxs.size());
    for (const size_t i : idxs) {
      entries.emplace_back(batch.at(i).id, batch.at(i).loc);
    }
    // One unsynced WriteBatch for the whole group, then a single fsync --
    // amortizing one WAL sync across every mapping in this flush cycle.
    mapper->putBatch(entries);
    mapper->syncWal();
    // The mappings are durable now; release each request's reactor so its
    // teardown drain (which waits pendingFlush_ to reach zero before destroying
    // the mapper) can proceed.
    for (const size_t i : idxs) {
      batch.at(i).pending->fetch_sub(1, std::memory_order_release);
    }
  }
}

void FlushWorker::run() {
  std::vector<FlushRequest> batch;
  batch.reserve(maxBatch_);
  while (!stop_.load(std::memory_order_acquire)) {
    if (drainInto(*requestQueue_, maxBatch_, batch) == 0) {
      if (busyPoll_) {
        // Hypernode-style busy-poll: pause instructions instead of sleep.
        // Matches ThreadMgr::threadWorker so this pool contributes to CPU
        // util the same way `hn.chunkMapper` writes do.
        folly::asm_volatile_pause();
        folly::asm_volatile_pause();
      } else {
        // Idle: brief sleep so workers don't busy-spin an empty queue (the
        // flush path is latency-tolerant, like Hypernode's *cooperative*
        // control path -- only the data-path pools spin).
        // @lint-ignore CLANGTIDY
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      continue;
    }
    processBatch(batch);
  }
  // Final drain after stop so no already-enqueued flush is dropped (and every
  // pending counter reaches zero).
  while (drainInto(*requestQueue_, maxBatch_, batch) > 0) {
    processBatch(batch);
  }
}

} // namespace facebook::halcyon
