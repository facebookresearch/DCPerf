// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstddef>

#include <folly/container/EvictingCacheMap.h>

#include "HalcyonChunkMapper.h" // ChunkId, ChunkLocation

namespace facebook::halcyon {

/// Per-reactor LRU cache of chunk-id -> location, mirroring HyperNode's
/// ChunkMetadataCache (fbcode/hypernode/control/mapper/ChunkMetadataCache.h): a
/// read-through + write-populate front end over the RocksDB chunk map, so hot
/// keys skip the DB lookup. As in HyperNode, the hit rate emerges from workload
/// access locality rather than prefetch.
///
/// Single-owner: built and used only by the reactor thread, like the reactor's
/// other per-thread state, so it needs no locking. Entry-count bounded (the
/// least-recently-used entry is evicted at capacity), the halcyon analog of
/// HyperNode's byte-bounded LRU.
class ChunkMetadataCache {
 public:
  /// `capacity` is the max number of entries retained; older entries are
  /// LRU-evicted beyond it. Clamped to at least 1.
  explicit ChunkMetadataCache(size_t capacity);

  /// Looks up `id`. On a hit, copies the location into `out`, marks `id`
  /// most-recently-used, and returns true; on a miss returns false (leaving
  /// `out` untouched).
  bool lookup(ChunkId id, ChunkLocation& out);

  /// Inserts or refreshes the mapping for `id`, marking it most-recently-used
  /// and evicting the least-recently-used entry if already at capacity.
  void insert(ChunkId id, const ChunkLocation& loc);

  /// Current number of cached entries.
  size_t size() const;

 private:
  folly::EvictingCacheMap<ChunkId, ChunkLocation> cache_;
};

} // namespace facebook::halcyon
