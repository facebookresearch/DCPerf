// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "HalcyonChunkMapper.h"

#include <unistd.h>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

namespace fs = std::filesystem;

// doDirectReads=false because /tmp is typically tmpfs, which rejects O_DIRECT.
HalcyonChunkMapper::Options testOpts() {
  HalcyonChunkMapper::Options o;
  o.doDirectReads = false;
  return o;
}

std::string uniqueDir(const std::string& tag) {
  return "/tmp/halcyon_chunkmap_test_" + tag + "_" + std::to_string(::getpid());
}

ChunkLocation makeLoc(uint32_t fileIndex, uint64_t offset, uint32_t length) {
  ChunkLocation loc;
  loc.fileIndex = fileIndex;
  loc.offset = offset;
  loc.length = length;
  loc.checksum = 0xABCD1234u;
  return loc;
}

bool sameLoc(const ChunkLocation& a, const ChunkLocation& b) {
  return a.fileIndex == b.fileIndex && a.length == b.length &&
      a.offset == b.offset && a.checksum == b.checksum &&
      a.reserved == b.reserved;
}

} // namespace

// serialize/deserialize round-trips, and rejects wrong-sized input.
TEST(HalcyonChunkMapperTest, Serialize_RoundTripAndRejectsBadSize) {
  const ChunkLocation in =
      makeLoc(/*fileIndex=*/7, /*offset=*/4096, /*length=*/8192);
  std::string bytes;
  serialize(in, bytes);
  EXPECT_EQ(bytes.size(), kChunkLocationSize);

  ChunkLocation out;
  ASSERT_TRUE(deserialize(bytes, out));
  EXPECT_TRUE(sameLoc(in, out));

  ChunkLocation ignored;
  EXPECT_FALSE(deserialize(bytes.substr(0, kChunkLocationSize - 1), ignored));
  EXPECT_FALSE(deserialize(bytes + "x", ignored));
}

// A fresh DB opens without throwing and starts with an empty key space.
TEST(HalcyonChunkMapperTest, Open_CreatesEmptyDb) {
  const std::string dir = uniqueDir("open");
  fs::remove_all(dir);

  HalcyonChunkMapper mapper(dir, testOpts());
  EXPECT_EQ(mapper.maxChunkId(), 0u);
  ChunkLocation loc;
  EXPECT_FALSE(mapper.lookup(0, loc));

  fs::remove_all(dir);
}

// A single put is read back identically.
TEST(HalcyonChunkMapperTest, PutGet_RoundTrips) {
  const std::string dir = uniqueDir("putget");
  fs::remove_all(dir);

  HalcyonChunkMapper mapper(dir, testOpts());
  const ChunkLocation in =
      makeLoc(/*fileIndex=*/3, /*offset=*/8192, /*length=*/4096);
  mapper.put(/*id=*/42, in);

  ChunkLocation out;
  ASSERT_TRUE(mapper.lookup(42, out));
  EXPECT_TRUE(sameLoc(in, out));

  fs::remove_all(dir);
}

// Looking up an absent key returns false rather than throwing.
TEST(HalcyonChunkMapperTest, Lookup_Missing_ReturnsFalse) {
  const std::string dir = uniqueDir("miss");
  fs::remove_all(dir);

  HalcyonChunkMapper mapper(dir, testOpts());
  mapper.put(/*id=*/1, makeLoc(0, 0, 4096));

  ChunkLocation out;
  EXPECT_FALSE(mapper.lookup(999, out));

  fs::remove_all(dir);
}

// A batch of mappings is all persisted and individually retrievable.
TEST(HalcyonChunkMapperTest, PutBatch_ThenLookupAll) {
  const std::string dir = uniqueDir("batch");
  fs::remove_all(dir);

  HalcyonChunkMapper mapper(dir, testOpts());
  std::vector<std::pair<ChunkId, ChunkLocation>> entries;
  entries.reserve(100);
  for (uint32_t i = 0; i < 100; ++i) {
    entries.emplace_back(
        i,
        makeLoc(/*fileIndex=*/i % 4, /*offset=*/i * 4096ull, /*length=*/4096));
  }
  mapper.putBatch(entries);

  for (const auto& [id, loc] : entries) {
    ChunkLocation out;
    ASSERT_TRUE(mapper.lookup(id, out)) << "missing id " << id;
    EXPECT_TRUE(sameLoc(loc, out));
  }

  fs::remove_all(dir);
}

// MultiGet reports hits and misses positionally.
TEST(HalcyonChunkMapperTest, MultiLookup_MixedHitsAndMisses) {
  const std::string dir = uniqueDir("multi");
  fs::remove_all(dir);

  HalcyonChunkMapper mapper(dir, testOpts());
  mapper.put(10, makeLoc(1, 4096, 4096));
  mapper.put(20, makeLoc(2, 8192, 8192));

  const std::vector<ChunkId> ids = {10, 11, 20};
  std::vector<ChunkLocation> out;
  std::vector<bool> found;
  mapper.multiLookup(ids, out, found);

  ASSERT_EQ(found.size(), 3u);
  EXPECT_TRUE(found[0]);
  EXPECT_FALSE(found[1]);
  EXPECT_TRUE(found[2]);
  EXPECT_TRUE(sameLoc(out[0], makeLoc(1, 4096, 4096)));
  EXPECT_TRUE(sameLoc(out[2], makeLoc(2, 8192, 8192)));

  fs::remove_all(dir);
}

// randomExistingKey stays within [0, maxId), and is 0 for an empty space.
TEST(HalcyonChunkMapperTest, RandomExistingKey_InRange) {
  EXPECT_EQ(HalcyonChunkMapper::randomExistingKey(0), 0u);
  for (int i = 0; i < 1000; ++i) {
    const ChunkId k = HalcyonChunkMapper::randomExistingKey(50);
    EXPECT_LT(k, 50u);
  }
}

// Mappings and the max chunk id survive a close + reopen of the same path.
TEST(HalcyonChunkMapperTest, Reopen_PersistsMappingsAndMaxChunkId) {
  const std::string dir = uniqueDir("reopen");
  fs::remove_all(dir);

  {
    HalcyonChunkMapper mapper(dir, testOpts());
    mapper.put(5, makeLoc(1, 4096, 4096));
    mapper.setMaxChunkId(6);
  }
  {
    HalcyonChunkMapper mapper(dir, testOpts());
    EXPECT_EQ(mapper.maxChunkId(), 6u);
    ChunkLocation out;
    ASSERT_TRUE(mapper.lookup(5, out));
    EXPECT_TRUE(sameLoc(out, makeLoc(1, 4096, 4096)));
  }

  fs::remove_all(dir);
}

// putBatch writes with sync=false (the default), so a single syncWal() is the
// amortized durability point for the whole batch. The mappings are readable
// after a close + reopen.
TEST(HalcyonChunkMapperTest, SyncWal_AfterPutBatch_PersistsAcrossReopen) {
  const std::string dir = uniqueDir("syncwal");
  fs::remove_all(dir);

  std::vector<std::pair<ChunkId, ChunkLocation>> entries;
  entries.reserve(16);
  for (uint32_t i = 0; i < 16; ++i) {
    entries.emplace_back(
        i,
        makeLoc(/*fileIndex=*/i % 4, /*offset=*/i * 4096ull, /*length=*/4096));
  }

  {
    HalcyonChunkMapper mapper(dir, testOpts());
    mapper.putBatch(entries);
    mapper.syncWal();
  }
  {
    HalcyonChunkMapper mapper(dir, testOpts());
    for (const auto& [id, loc] : entries) {
      ChunkLocation out;
      ASSERT_TRUE(mapper.lookup(id, out)) << "missing id " << id;
      EXPECT_TRUE(sameLoc(loc, out));
    }
  }

  fs::remove_all(dir);
}

} // namespace facebook::halcyon
