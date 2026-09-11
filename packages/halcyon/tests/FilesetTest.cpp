// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "Fileset.h"

#include "Common.h" // IoSizeRange, geometry constants
#include "HalcyonChunkMapper.h"

#include <unistd.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

namespace fs = std::filesystem;

std::string uniqueDir(const std::string& tag) {
  return "/tmp/halcyon_fileset_test_" + tag + "_" + std::to_string(::getpid());
}

// A single fixed I/O size: randomIoSize returns upperBound exactly when
// lowerBound == 0 (see Common.cpp), so chunks are exactly `bytes` long.
IoSizeRange fixedSize(uint64_t bytes) {
  IoSizeRange r;
  r.upperBound = bytes;
  r.percentage = 100;
  return r;
}

} // namespace

TEST(CommonTest, ParseIoSizeAcceptsLowercaseBinarySuffixes) {
  EXPECT_EQ(parseIoSize("1k", 100).upperBound, 1024);
  EXPECT_EQ(parseIoSize("1m", 100).upperBound, 1024 * 1024);
  EXPECT_EQ(parseIoSize("1g", 100).upperBound, 1024 * 1024 * 1024);
}

TEST(CommonTest, HumanToIntAcceptsEmptyInput) {
  EXPECT_EQ(humanToInt(""), 0);
}

TEST(FilesetTest, Create_DistributesConfiguredDirectoriesAcrossWorkers) {
  const std::string dir = uniqueDir("create");
  fs::remove_all(dir);
  fs::create_directories(dir);

  CreateStats stats(/*operators=*/1, /*workers=*/2);
  Fileset fileset(
      /*operatorID=*/0,
      dir,
      /*dirs=*/2,
      /*files=*/1,
      /*threadsPerDir=*/2);
  EXPECT_EQ(fileset.create(&stats), 0);

  std::ifstream manifest(dir + "/manifest");
  std::string manifestData;
  std::getline(manifest, manifestData);
  EXPECT_EQ(manifestData, "2 1");
  EXPECT_EQ(fs::file_size(dir + "/d0/f0"), kMaxFileSize);
  EXPECT_EQ(fs::file_size(dir + "/d1/f0"), kMaxFileSize);
  EXPECT_FALSE(fs::exists(dir + "/d2"));

  fs::remove_all(dir);
}

// populateChunkMap lays out fixed-size chunks contiguously in each data file
// and records a valid, non-overlapping location for every chunk over a
// contiguous chunk-id space.
TEST(FilesetTest, PopulateChunkMap_FixedSize_ContiguousValidLayout) {
  const std::string dir = uniqueDir("populate");
  fs::remove_all(dir);
  fs::create_directories(dir);

  const int dirs = 1;
  const int files = 2;
  const uint64_t chunkLen = 256 * static_cast<uint64_t>(kMinIoSize); // 1 MiB
  populateChunkMap(
      dir, dirs, files, {fixedSize(chunkLen)}, /*doDirectIo=*/false);

  HalcyonChunkMapper::Options opts;
  opts.doDirectReads = false;
  HalcyonChunkMapper mapper(dir + "/chunkmap.rocksdb", opts);

  // Chunks per file derived independently of the implementation: chunks start
  // past the header and tile the file by chunkLen.
  const int numFiles = dirs * files;
  const uint64_t perFile = (kMaxFileSize - kChunkHeaderSize) / chunkLen;
  ASSERT_GT(perFile, 0u);
  EXPECT_EQ(mapper.maxChunkId(), perFile * numFiles);

  // Walk the whole contiguous id space; every chunk resolves to an in-bounds,
  // sector-aligned extent that tiles its file with no gaps or overlaps.
  std::vector<uint64_t> nextOffset(numFiles, kChunkHeaderSize);
  std::vector<uint64_t> countPerFile(numFiles, 0);
  for (ChunkId id = 0; id < mapper.maxChunkId(); ++id) {
    ChunkLocation loc;
    ASSERT_TRUE(mapper.lookup(id, loc)) << "missing id " << id;
    ASSERT_LT(loc.fileIndex, static_cast<uint32_t>(numFiles));
    EXPECT_EQ(loc.length, static_cast<uint32_t>(chunkLen));
    EXPECT_EQ(loc.offset % kSectorSize, 0u);
    EXPECT_LE(loc.offset + loc.length, static_cast<uint64_t>(kMaxFileSize));
    // Contiguous, non-overlapping within the file.
    EXPECT_EQ(loc.offset, nextOffset[loc.fileIndex]);
    nextOffset[loc.fileIndex] += loc.length;
    countPerFile[loc.fileIndex]++;
  }
  for (int f = 0; f < numFiles; ++f) {
    EXPECT_EQ(countPerFile[f], perFile);
  }

  fs::remove_all(dir);
}

// An empty read-size distribution is a setup error, surfaced as an exception.
TEST(FilesetTest, PopulateChunkMap_EmptyReadSizes_Throws) {
  const std::string dir = uniqueDir("empty_sizes");
  fs::remove_all(dir);
  fs::create_directories(dir);

  EXPECT_THROW(
      populateChunkMap(dir, /*dirs=*/1, /*files=*/1, {}, /*doDirectIo=*/false),
      std::runtime_error);

  fs::remove_all(dir);
}

} // namespace facebook::halcyon
