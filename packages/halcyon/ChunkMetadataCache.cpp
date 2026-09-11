// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ChunkMetadataCache.h"

#include <algorithm>

namespace facebook::halcyon {

ChunkMetadataCache::ChunkMetadataCache(size_t capacity)
    : cache_(std::max<size_t>(1, capacity)) {}

bool ChunkMetadataCache::lookup(ChunkId id, ChunkLocation& out) {
  // find() promotes the entry to most-recently-used on a hit -- the
  // read-through recency signal that lets locality drive the hit rate.
  auto it = cache_.find(id);
  if (it == cache_.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void ChunkMetadataCache::insert(ChunkId id, const ChunkLocation& loc) {
  // set() overwrites an existing mapping and promotes it; at capacity it evicts
  // the least-recently-used entry.
  cache_.set(id, loc);
}

size_t ChunkMetadataCache::size() const {
  return cache_.size();
}

} // namespace facebook::halcyon
