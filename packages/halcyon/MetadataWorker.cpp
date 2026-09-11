// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "MetadataWorker.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <folly/container/F14Map.h>
#include <folly/portability/Asm.h>
#include <glog/logging.h> // CHECK

#include "Common.h" // current_nano

namespace facebook::halcyon {

namespace {

// Reads up to maxBatch requests (non-blocking) into `batch` (cleared first);
// returns the number drained.
size_t drainInto(
    LookupRequestQueue& q,
    size_t maxBatch,
    std::vector<LookupRequest>& batch) {
  batch.clear();
  LookupRequest req;
  while (batch.size() < maxBatch && q.read(req)) {
    batch.push_back(req);
  }
  return batch.size();
}

} // namespace

MetadataWorker::MetadataWorker(
    LookupRequestQueue* requestQueue,
    std::vector<LookupResultQueue*> resultQueues,
    size_t maxBatch,
    bool busyPoll)
    : requestQueue_(requestQueue),
      resultQueues_(std::move(resultQueues)),
      maxBatch_(std::max<size_t>(1, maxBatch)),
      busyPoll_(busyPoll) {}

void MetadataWorker::stop() {
  stop_.store(true, std::memory_order_release);
}

void MetadataWorker::processBatch(std::vector<LookupRequest>& batch) {
  // Group by mapper so each MultiGet hits a single RocksDB instance (mirrors
  // HyperNode batching lookups per partition).
  folly::F14FastMap<HalcyonChunkMapper*, std::vector<size_t>> byMapper;
  for (size_t i = 0; i < batch.size(); ++i) {
    byMapper[batch.at(i).mapper].push_back(i);
  }

  std::vector<ChunkId> ids;
  std::vector<ChunkLocation> locs;
  std::vector<bool> found;
  for (auto& [mapper, idxs] : byMapper) {
    ids.clear();
    ids.reserve(idxs.size());
    for (const size_t i : idxs) {
      ids.push_back(batch.at(i).id);
    }
    // Time the batched MultiGet and split it evenly across its requests so each
    // result carries its share of the metadata-read cost.
    const uint64_t start = current_nano();
    mapper->multiLookup(ids, locs, found);
    const uint64_t perRequestNanos =
        idxs.empty() ? 0 : (current_nano() - start) / idxs.size();
    for (size_t j = 0; j < idxs.size(); ++j) {
      const LookupRequest& r = batch.at(idxs.at(j));
      LookupResult res;
      res.slotIndex = r.slotIndex;
      res.found = found.at(j);
      res.lookupNanos = perRequestNanos;
      if (found.at(j)) {
        res.loc = locs.at(j);
      }
      // reactorId is validated by the engine when it builds the result-queue
      // vector; .at() keeps the lookup bounds-checked regardless. IoUringEngine
      // oversizes the result queue well above the at-most-one-outstanding-
      // lookup-per-slot maximum to stay clear of DMPSCQueue's approximate
      // capacity (folly reserves slack + per-producer credit), so try_enqueue
      // can't fail here -- CHECK documents that invariant.
      CHECK(resultQueues_.at(r.reactorId)->try_enqueue(res));
    }
  }
}

void MetadataWorker::run() {
  std::vector<LookupRequest> batch;
  batch.reserve(maxBatch_);
  while (!stop_.load(std::memory_order_acquire)) {
    if (drainInto(*requestQueue_, maxBatch_, batch) == 0) {
      if (busyPoll_) {
        // Hypernode-style busy-poll (matches `hn.chunkMapper` reads).
        folly::asm_volatile_pause();
        folly::asm_volatile_pause();
      } else {
        // Idle: brief sleep. Hypernode's *cooperative* control path pattern
        // (only its data-path pools spin).
        // @lint-ignore CLANGTIDY
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      continue;
    }
    processBatch(batch);
  }
  // Final drain after stop so no already-enqueued request is dropped.
  while (drainInto(*requestQueue_, maxBatch_, batch) > 0) {
    processBatch(batch);
  }
}

} // namespace facebook::halcyon
