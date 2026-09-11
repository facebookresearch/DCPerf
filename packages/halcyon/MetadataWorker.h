// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <folly/MPMCQueue.h>
#include <folly/concurrency/DynamicBoundedQueue.h>

#include "HalcyonChunkMapper.h" // ChunkId, ChunkLocation

namespace facebook::halcyon {

/// A chunk-map lookup enqueued by a reactor for the QMetadata pool to resolve
/// off the I/O thread. Carries the originating reactor's mapper (per-mount DB)
/// plus where to deliver the answer.
struct LookupRequest {
  HalcyonChunkMapper* mapper{nullptr}; // which mount's chunk map to query
  ChunkId id{0}; // chunk id to resolve
  int reactorId{0}; // index of the originating reactor (into the result queues)
  int slotIndex{0}; // op slot awaiting the result
};

/// A resolved lookup, delivered back to the originating reactor's result queue.
struct LookupResult {
  int slotIndex{0};
  ChunkLocation loc;
  bool found{false};
  // Per-request share of the batched MultiGet wall time (ns). The reactor folds
  // this into MetadataStats on its own thread, so all PerfStats writes stay
  // single-threaded even though the lookup ran on a worker.
  uint64_t lookupNanos{0};
};

/// Shared request queue (reactors -> workers): MPMC, since many reactors
/// produce requests and many workers consume them.
using LookupRequestQueue = folly::MPMCQueue<LookupRequest>;
/// Per-reactor result queue (workers -> one reactor): multiple worker producers
/// but a SINGLE consumer (the owning reactor's drainLookupResults), so a
/// bounded MPSC queue avoids the multi-consumer dequeue synchronization
/// MPMCQueue would pay on every reaped result. MayBlock=false: capacity is
/// sized to the reactor's slot count (one outstanding lookup per slot), so
/// try_enqueue never overflows.
using LookupResultQueue = folly::DMPSCQueue<LookupResult, /*MayBlock=*/false>;

/// Drains lookup requests from a shared queue, batches them per mapper into one
/// MultiGet, and posts each result to the originating reactor's result queue.
/// Mirrors HyperNode's RocksKVStore lookup workers (shared pool, batched
/// MultiGet, queue handoff).
///
/// A single instance is shared by all worker threads in the QMetadata pool:
/// each thread calls run(). The shared queues are thread-safe and
/// HalcyonChunkMapper's reads are safe under concurrent MultiGet, so no
/// additional locking is needed. stop() ends the loops after draining whatever
/// is already queued.
class MetadataWorker {
 public:
  /// `requestQueue` and `resultQueues` (indexed by reactorId) are owned by the
  /// caller and must outlive the worker. `maxBatch` caps requests per MultiGet.
  /// `busyPoll` swaps the idle-loop 50us sleep for `folly::asm_volatile_pause`,
  /// matching Hypernode's ThreadMgr::threadWorker busy-poll pattern
  /// (100% CPU per thread even when the queue is empty).
  MetadataWorker(
      LookupRequestQueue* requestQueue,
      std::vector<LookupResultQueue*> resultQueues,
      size_t maxBatch,
      bool busyPoll = false);

  /// Worker loop; runs until stop(), then drains remaining requests. Submit one
  /// invocation per QMetadata pool thread.
  void run();

  /// Signals the loops to finish (after draining what is already queued).
  void stop();

 private:
  void processBatch(std::vector<LookupRequest>& batch);

  LookupRequestQueue* requestQueue_;
  std::vector<LookupResultQueue*> resultQueues_;
  size_t maxBatch_;
  bool busyPoll_;
  std::atomic<bool> stop_{false};
};

} // namespace facebook::halcyon
