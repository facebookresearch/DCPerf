// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include <folly/MPMCQueue.h>

#include "HalcyonChunkMapper.h" // ChunkId, ChunkLocation

namespace facebook::halcyon {

/// A chunk-map write enqueued by a reactor for the QFlush pool to persist off
/// the I/O thread. Carries the originating reactor's mapper (per-mount DB), the
/// mapping to write, and a pointer to that reactor's outstanding-flush counter
/// so the worker can signal completion.
struct FlushRequest {
  HalcyonChunkMapper* mapper{nullptr}; // which mount's chunk map to write
  ChunkId id{0}; // chunk id to persist
  ChunkLocation loc; // physical location to map it to
  // The originating reactor's pending-flush counter; the worker decrements it
  // once the mapping is durable, so the reactor can wait its flushes out before
  // destroying its mapper at teardown.
  std::atomic<int>* pending{nullptr};
};

/// Shared request queue (reactors -> workers): MPMC, since many reactors
/// produce flush requests and many workers consume them.
using FlushRequestQueue = folly::MPMCQueue<FlushRequest>;

/// Drains flush requests from a shared queue, batches them per mapper into one
/// putBatch + a single amortized syncWal, then signals each request's reactor
/// (decrements its pending counter). Mirrors HyperNode's flush workers: shared
/// pool, batched write-back, amortized WAL sync off the data-path core.
///
/// A single instance is shared by all worker threads in the QFlush pool: each
/// thread calls run(). The shared queue is thread-safe and HalcyonChunkMapper's
/// writes are safe under concurrent putBatch/syncWal, so no additional locking
/// is needed. stop() ends the loops after draining whatever is already queued.
class FlushWorker {
 public:
  /// `requestQueue` is owned by the caller and must outlive the worker.
  /// `maxBatch` caps requests per putBatch.
  /// `busyPoll` swaps the idle-loop 50us sleep for `folly::asm_volatile_pause`,
  /// matching Hypernode's ThreadMgr::threadWorker pattern (100% CPU per
  /// thread even when the queue is empty).
  FlushWorker(
      FlushRequestQueue* requestQueue,
      size_t maxBatch,
      bool busyPoll = false);

  /// Worker loop; runs until stop(), then drains remaining requests. Submit one
  /// invocation per QFlush pool thread.
  void run();

  /// Signals the loops to finish (after draining what is already queued).
  void stop();

 private:
  void processBatch(std::vector<FlushRequest>& batch);

  FlushRequestQueue* requestQueue_;
  size_t maxBatch_;
  bool busyPoll_;
  std::atomic<bool> stop_{false};
};

} // namespace facebook::halcyon
