// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ChunkMetadataCache.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

// Distinct, id-derived location so a hit can be checked against the value that
// was inserted (independently constructed, not read back from the cache).
ChunkLocation makeLoc(ChunkId id) {
  ChunkLocation loc;
  loc.fileIndex = static_cast<uint32_t>(id % 7);
  loc.length = static_cast<uint32_t>(4096 + id);
  loc.offset = 1024 * (id + 1);
  return loc;
}

void expectSameLoc(const ChunkLocation& got, const ChunkLocation& want) {
  EXPECT_EQ(got.fileIndex, want.fileIndex);
  EXPECT_EQ(got.length, want.length);
  EXPECT_EQ(got.offset, want.offset);
}

} // namespace

// A populated key returns its stored location; an absent key misses without
// touching the out-param.
TEST(ChunkMetadataCacheTest, Lookup_HitReturnsStoredLocation_MissReturnsFalse) {
  ChunkMetadataCache cache(/*capacity=*/4);
  cache.insert(3, makeLoc(3));

  ChunkLocation got;
  EXPECT_TRUE(cache.lookup(3, got));
  expectSameLoc(got, makeLoc(3));

  ChunkLocation untouched = makeLoc(99);
  EXPECT_FALSE(cache.lookup(42, untouched));
  // Miss leaves the caller's buffer alone.
  expectSameLoc(untouched, makeLoc(99));
}

// At capacity, inserting a new key evicts the least-recently-used one. A lookup
// counts as a use, so it protects an otherwise-oldest key from eviction.
TEST(ChunkMetadataCacheTest, Insert_AtCapacity_EvictsLeastRecentlyUsed) {
  ChunkMetadataCache cache(/*capacity=*/2);
  cache.insert(0, makeLoc(0));
  cache.insert(1, makeLoc(1));

  // Touch 0 so 1 becomes the least-recently-used; the next insert evicts 1.
  ChunkLocation got;
  ASSERT_TRUE(cache.lookup(0, got));
  cache.insert(2, makeLoc(2));

  EXPECT_EQ(cache.size(), 2u);
  EXPECT_FALSE(cache.lookup(1, got)); // evicted
  EXPECT_TRUE(cache.lookup(0, got)); // protected by the earlier lookup
  EXPECT_TRUE(cache.lookup(2, got)); // just inserted
}

// Re-inserting an existing key overwrites its location rather than adding a
// second entry.
TEST(ChunkMetadataCacheTest, Insert_ExistingKey_OverwritesLocation) {
  ChunkMetadataCache cache(/*capacity=*/4);
  cache.insert(5, makeLoc(5));
  cache.insert(5, makeLoc(6)); // same key, different location

  EXPECT_EQ(cache.size(), 1u);
  ChunkLocation got;
  ASSERT_TRUE(cache.lookup(5, got));
  expectSameLoc(got, makeLoc(6));
}

} // namespace facebook::halcyon
