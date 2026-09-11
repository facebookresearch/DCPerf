// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "IoUringEngine.h"

#include "Common.h" // kMinIoSize, kMaxFileSize, IoSizeRange
#include "HalcyonChunkMapper.h"
#include "MetadataWorker.h"
#include "PerfStats.h"
#include "ThreadPools.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::halcyon {
namespace {

namespace fs = std::filesystem;

/// Creates a minimal Halcyon fileset under `baseDir`: a `manifest` holding
/// "<dirs> <files>" (no trailing newline, matching readManifest) plus
/// dirs*files data files at d{d}/f{f}, each sized `fileSize` bytes (sparse).
void makeFileset(
    const std::string& baseDir,
    int dirs,
    int files,
    uintmax_t fileSize = 0) {
  fs::remove_all(baseDir);
  fs::create_directories(baseDir);
  {
    std::ofstream manifest(baseDir + "/manifest");
    manifest << dirs << " " << files;
  }
  for (int d = 0; d < dirs; ++d) {
    fs::create_directories(baseDir + "/d" + std::to_string(d));
    for (int f = 0; f < files; ++f) {
      const std::string path =
          baseDir + "/d" + std::to_string(d) + "/f" + std::to_string(f);
      {
        std::ofstream create(path);
      }
      if (fileSize > 0) {
        fs::resize_file(path, fileSize);
      }
    }
  }
}

std::string uniqueDir(const std::string& tag) {
  return "/tmp/halcyon_iouring_test_" + tag + "_" + std::to_string(::getpid());
}

// doDirectIo=false everywhere because /tmp is typically tmpfs, which rejects
// O_DIRECT. Both read and write use a single fixed 4K size for deterministic
// byte-count assertions. iops=0 (default) leaves the reactor unthrottled.
ReactorConfig makeReactorConfig(
    const std::string& dir,
    int queueDepth,
    double readRatio = 1.0,
    double iops = 0.0,
    MetadataBackend backend = MetadataBackend::Manifest) {
  ReactorConfig rc;
  rc.baseDir = dir;
  rc.operatorId = 0;
  rc.queueDepth = queueDepth;
  rc.ringEntries = 8;
  rc.doDirectIo = false;
  rc.readRatio = readRatio;
  rc.iops = iops;
  rc.backend = backend;
  IoSizeRange fourK;
  fourK.upperBound = kMinIoSize; // single fixed 4K size
  fourK.percentage = 100;
  rc.readSizes = {fourK};
  rc.writeSizes = {fourK};
  return rc;
}

// Opens the RocksDB chunk map the reactor uses for `dir` (doDirectReads=false
// for tmpfs).
HalcyonChunkMapper openChunkMap(const std::string& dir) {
  HalcyonChunkMapper::Options opts;
  opts.doDirectReads = false;
  return HalcyonChunkMapper(dir + "/chunkmap.rocksdb", opts);
}

// Populates `dir`'s chunk map with `numChunks` sequential ids, each mapped to a
// distinct 4K extent in file 0 past the header, and records the key range.
void populateChunkMap(const std::string& dir, ChunkId numChunks) {
  HalcyonChunkMapper mapper = openChunkMap(dir);
  std::vector<std::pair<ChunkId, ChunkLocation>> entries;
  entries.reserve(numChunks);
  for (ChunkId id = 0; id < numChunks; ++id) {
    ChunkLocation loc;
    loc.fileIndex = 0;
    loc.length = kMinIoSize;
    loc.offset = kChunkHeaderSize + id * static_cast<uint64_t>(kMinIoSize);
    entries.emplace_back(id, loc);
  }
  mapper.putBatch(entries);
  mapper.setMaxChunkId(numChunks);
}

} // namespace

// openFiles() reads the manifest and opens one fd per file in the set.
TEST(IoUringEngineTest, OpenFiles_OpensEveryFileInManifest) {
  const std::string dir = uniqueDir("open");
  makeFileset(dir, /*dirs=*/2, /*files=*/3);

  IoCoreReactor reactor(makeReactorConfig(dir, /*queueDepth=*/4), nullptr);
  reactor.openFiles();

  EXPECT_EQ(reactor.fds().size(), 6u); // 2 dirs * 3 files
  for (const int fd : reactor.fds()) {
    EXPECT_GE(fd, 0);
  }

  fs::remove_all(dir);
}

// allocateSlots() makes queueDepth slots, each with an aligned buffer and Idle
// state.
TEST(IoUringEngineTest, AllocateSlots_MakesAlignedIdleBuffers) {
  const std::string dir = uniqueDir("slots");
  makeFileset(dir, 1, 1);

  IoCoreReactor reactor(makeReactorConfig(dir, /*queueDepth=*/3), nullptr);
  reactor.allocateSlots();

  ASSERT_EQ(reactor.slots().size(), 3u);
  for (const auto& slot : reactor.slots()) {
    ASSERT_NE(slot.buffer, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(slot.buffer) % kMinIoSize, 0u);
    EXPECT_EQ(slot.state, SlotState::Idle);
  }

  fs::remove_all(dir);
}

// A missing manifest is a setup error, surfaced as an exception (not a crash).
TEST(IoUringEngineTest, OpenFiles_MissingManifestThrows) {
  const std::string dir = uniqueDir("nomanifest");
  fs::remove_all(dir);
  fs::create_directories(dir); // dir exists, but no manifest inside

  IoCoreReactor reactor(makeReactorConfig(dir, /*queueDepth=*/1), nullptr);
  EXPECT_THROW(reactor.openFiles(), std::runtime_error);

  fs::remove_all(dir);
}

// The read path (readRatio=1.0) drives reads through the ring and records I/O +
// inline-checksum stats. Runs for a short fixed window against a full-size
// sparse file.
TEST(IoUringEngineTest, Run_ReadPath_RecordsIoAndChecksumStats) {
  const std::string dir = uniqueDir("read");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoCoreReactor reactor(
      makeReactorConfig(dir, queueDepth, /*readRatio=*/1.0), &perf);

  const uint64_t now = current_nano();
  reactor.run(/*warmupEndNs=*/now,
              /*deadlineNs=*/now + 200'000'000ull); // 200ms

  uint64_t totalReadIos = 0;
  uint64_t totalReadBytes = 0;
  uint64_t totalChecksums = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalReadIos += perf.istats[0][w].readIos;
    totalReadBytes += perf.istats[0][w].readBytes;
    totalChecksums += perf.cstats[0][w].readCount;
  }

  EXPECT_GT(totalReadIos, 0u);
  // Every read is a fixed 4K within a kMaxFileSize file, so it always fills.
  EXPECT_EQ(totalReadBytes, totalReadIos * static_cast<uint64_t>(kMinIoSize));
  EXPECT_GT(totalChecksums, 0u);

  // Manifest backend does no metadata ops, so the metadata path stays zero.
  uint64_t totalMetaReads = 0;
  uint64_t totalMetaWrites = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalMetaReads += perf.mstats[0][w].readCount;
    totalMetaWrites += perf.mstats[0][w].writeCount;
  }
  EXPECT_EQ(totalMetaReads, 0u);
  EXPECT_EQ(totalMetaWrites, 0u);

  // All slots are drained back to Idle before teardown.
  for (const auto& slot : reactor.slots()) {
    EXPECT_EQ(slot.state, SlotState::Idle);
  }

  fs::remove_all(dir);
}

// The write path (readRatio=0.0) drives writes through the ring and records
// write I/O + inline-checksum stats.
TEST(IoUringEngineTest, Run_WritePath_RecordsIoAndChecksumStats) {
  const std::string dir = uniqueDir("write");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoCoreReactor reactor(
      makeReactorConfig(dir, queueDepth, /*readRatio=*/0.0), &perf);

  const uint64_t now = current_nano();
  reactor.run(/*warmupEndNs=*/now,
              /*deadlineNs=*/now + 200'000'000ull); // 200ms

  uint64_t totalWriteIos = 0;
  uint64_t totalWriteBytes = 0;
  uint64_t totalChecksums = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalWriteIos += perf.istats[0][w].writeIos;
    totalWriteBytes += perf.istats[0][w].writeBytes;
    totalChecksums += perf.cstats[0][w].writeCount;
  }

  EXPECT_GT(totalWriteIos, 0u);
  // Each write is the pure on-disk size of a 4K request == 4K.
  EXPECT_EQ(totalWriteBytes, totalWriteIos * static_cast<uint64_t>(kMinIoSize));
  EXPECT_GT(totalChecksums, 0u);

  for (const auto& slot : reactor.slots()) {
    EXPECT_EQ(slot.state, SlotState::Idle);
  }

  fs::remove_all(dir);
}

// Pacing: with iops set, the reactor throttles op submission to the target rate
// (spread across slots), so it issues far fewer ops than an unthrottled reactor
// would in the same window.
TEST(IoUringEngineTest, Run_Pacing_ThrottlesToIops) {
  const std::string dir = uniqueDir("pace");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 1;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  // iops=200 with queueDepth=1 -> ~5ms between op starts -> ~60 ops in 300ms,
  // versus the thousands an unthrottled reactor would do.
  IoCoreReactor reactor(
      makeReactorConfig(dir, queueDepth, /*readRatio=*/1.0, /*iops=*/200.0),
      &perf);

  const uint64_t now = current_nano();
  reactor.run(/*warmupEndNs=*/now,
              /*deadlineNs=*/now + 300'000'000ull); // 300ms

  const uint64_t ios = perf.istats[0][0].readIos;
  EXPECT_GT(ios, 10u); // made meaningful progress near the target rate
  EXPECT_LT(ios, 300u); // throttled: far below unthrottled throughput

  fs::remove_all(dir);
}

// Full engine lifecycle (zero duration) over real filesets: each reactor sets
// up + tears down cleanly without hanging, even with no time to do I/O.
TEST(IoUringEngineTest, Run_ZeroDuration_SetsUpAndTearsDown) {
  const std::string dirA = uniqueDir("run_a");
  const std::string dirB = uniqueDir("run_b");
  makeFileset(dirA, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);
  makeFileset(dirB, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  HalcyonPools pools = buildPools(PoolConfig{.qioThreads = 2});

  PerfStats perf(/*operators=*/2, /*workers=*/4);
  IoSizeRange fourK;
  fourK.upperBound = kMinIoSize;
  fourK.percentage = 100;

  IoUringEngine::Config config;
  config.mountPoints = {dirA, dirB};
  config.warmupSec = 0;
  config.runtimeSec = 0;
  config.threadsPerDisk = 4;
  config.doDirectIo = false;
  config.readSizes = {fourK};
  config.writeSizes = {fourK};
  config.perfStats = &perf;

  IoUringEngine engine(pools, std::move(config));
  // Zero-duration setup/teardown must not throw or hang; reactors do no I/O.
  EXPECT_NO_THROW(engine.run());

  fs::remove_all(dirA);
  fs::remove_all(dirB);
}

// RocksDb backend, write path (readRatio=0): writes bump-allocate offsets and
// persist a chunk-id -> location mapping per completed write. After the run the
// reactor-owned chunk map holds a non-empty, valid key range.
TEST(IoUringEngineTest, Run_RocksDbWritePath_PopulatesMapper) {
  const std::string dir = uniqueDir("rocks_write");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  {
    IoCoreReactor reactor(
        makeReactorConfig(
            dir,
            queueDepth,
            /*readRatio=*/0.0,
            /*iops=*/0.0,
            MetadataBackend::RocksDb),
        &perf);
    const uint64_t now = current_nano();
    reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 200'000'000ull);
  }

  uint64_t totalWriteIos = 0;
  uint64_t totalMetaWrites = 0;
  uint64_t totalMetaReads = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalWriteIos += perf.istats[0][w].writeIos;
    totalMetaWrites += perf.mstats[0][w].writeCount;
    totalMetaReads += perf.mstats[0][w].readCount;
  }
  EXPECT_GT(totalWriteIos, 0u);
  // The metadata write path is measured: a put is timed per completed write,
  // and the write-only run does no lookups.
  EXPECT_EQ(totalMetaWrites, totalWriteIos);
  EXPECT_EQ(totalMetaReads, 0u);

  // Reopen the chunk map the reactor persisted: it records a non-empty key
  // range and the stored locations are valid 4K extents past the header.
  HalcyonChunkMapper mapper = openChunkMap(dir);
  ASSERT_GT(mapper.maxChunkId(), 0u);
  ChunkLocation loc;
  ASSERT_TRUE(mapper.lookup(0, loc));
  EXPECT_EQ(loc.fileIndex, 0u);
  EXPECT_EQ(loc.length, static_cast<uint32_t>(kMinIoSize));
  EXPECT_GE(loc.offset, static_cast<uint64_t>(kChunkHeaderSize));

  fs::remove_all(dir);
}

// RocksDb backend, read path (readRatio=1) over a pre-populated chunk map:
// every read looks up a stored location and reads exactly its length, so byte
// counts are deterministic.
TEST(IoUringEngineTest, Run_RocksDbReadPath_ReadsMappedLocations) {
  const std::string dir = uniqueDir("rocks_read");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  constexpr ChunkId kNumChunks = 8;
  {
    HalcyonChunkMapper mapper = openChunkMap(dir);
    std::vector<std::pair<ChunkId, ChunkLocation>> entries;
    entries.reserve(kNumChunks);
    for (ChunkId id = 0; id < kNumChunks; ++id) {
      ChunkLocation loc;
      loc.fileIndex = 0;
      loc.length = kMinIoSize;
      loc.offset = kChunkHeaderSize + id * static_cast<uint64_t>(kMinIoSize);
      entries.emplace_back(id, loc);
    }
    mapper.putBatch(entries);
    mapper.setMaxChunkId(kNumChunks);
  }

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  {
    IoCoreReactor reactor(
        makeReactorConfig(
            dir,
            queueDepth,
            /*readRatio=*/1.0,
            /*iops=*/0.0,
            MetadataBackend::RocksDb),
        &perf);
    const uint64_t now = current_nano();
    reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 200'000'000ull);
  }

  uint64_t totalReadIos = 0;
  uint64_t totalReadBytes = 0;
  uint64_t totalMetaReads = 0;
  uint64_t totalMetaWrites = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalReadIos += perf.istats[0][w].readIos;
    totalReadBytes += perf.istats[0][w].readBytes;
    totalMetaReads += perf.mstats[0][w].readCount;
    totalMetaWrites += perf.mstats[0][w].writeCount;
  }
  EXPECT_GT(totalReadIos, 0u);
  EXPECT_EQ(totalReadBytes, totalReadIos * static_cast<uint64_t>(kMinIoSize));
  // The metadata read path is measured: every chunk is pre-populated so every
  // read is a lookup hit -> one metadata read per disk read, and no puts.
  EXPECT_EQ(totalMetaReads, totalReadIos);
  EXPECT_EQ(totalMetaWrites, 0u);

  fs::remove_all(dir);
}

// RocksDb read path with the per-reactor cache enabled: over a key space that
// fits in the cache, reads are read-through (first touch of an id misses and
// populates, repeats hit), so the cache produces hits and the hit/miss
// breakdown partitions every counted read.
TEST(IoUringEngineTest, Run_RocksDbReadPath_CacheProducesHits) {
  const std::string dir = uniqueDir("rocks_cache");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  constexpr ChunkId kNumChunks = 8;
  populateChunkMap(dir, kNumChunks);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  {
    ReactorConfig rc = makeReactorConfig(
        dir,
        queueDepth,
        /*readRatio=*/1.0,
        /*iops=*/0.0,
        MetadataBackend::RocksDb);
    // Capacity exceeds the key space, so nothing is evicted: each id misses at
    // most once and every repeat hits.
    rc.cacheCapacity = 64;
    IoCoreReactor reactor(std::move(rc), &perf);
    const uint64_t now = current_nano();
    reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 200'000'000ull);
  }

  uint64_t readCount = 0;
  uint64_t readHits = 0;
  uint64_t readMisses = 0;
  for (int w = 0; w < queueDepth; ++w) {
    readCount += perf.mstats[0][w].readCount;
    readHits += perf.mstats[0][w].readHits;
    readMisses += perf.mstats[0][w].readMisses;
  }
  // The breakdown partitions every counted read.
  EXPECT_EQ(readCount, readHits + readMisses);
  // Reuse over the cached key space produces hits...
  EXPECT_GT(readHits, 0u);
  // ...and with capacity >= key space, each distinct id misses at most once, so
  // total misses can't exceed the number of ids.
  EXPECT_LE(readMisses, kNumChunks);

  fs::remove_all(dir);
}

// Async (QMetadata) read path with the per-reactor cache enabled: the reactor
// probes the cache before offloading, so only the cold first touch of each id
// goes to a worker (and populates the cache); repeats are served from cache and
// never enqueued. Hits are produced and the worker round-trips (misses) stay
// bounded by the key space, demonstrating the cache short-circuits the offload.
TEST(IoUringEngineTest, Run_RocksDbAsyncReadPath_CacheShortCircuitsWorker) {
  const std::string dir = uniqueDir("rocks_async_cache");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  constexpr ChunkId kNumChunks = 8;
  populateChunkMap(dir, kNumChunks);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);

  LookupRequestQueue requestQueue(queueDepth);
  LookupResultQueue resultQueue(queueDepth);
  MetadataWorker worker(&requestQueue, {&resultQueue}, /*maxBatch=*/queueDepth);
  std::thread workerThread([&worker] { worker.run(); });

  {
    ReactorConfig rc = makeReactorConfig(
        dir,
        queueDepth,
        /*readRatio=*/1.0,
        /*iops=*/0.0,
        MetadataBackend::RocksDb);
    rc.requestQueue = &requestQueue;
    rc.resultQueue = &resultQueue;
    // Capacity exceeds the key space, so once an id is cached it is never
    // evicted -- every repeat is a cache hit served without the worker.
    rc.cacheCapacity = 64;
    IoCoreReactor reactor(std::move(rc), &perf);
    const uint64_t now = current_nano();
    reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 200'000'000ull);
  }
  worker.stop();
  workerThread.join();

  uint64_t readIos = 0;
  uint64_t readCount = 0;
  uint64_t readHits = 0;
  uint64_t readMisses = 0;
  for (int w = 0; w < queueDepth; ++w) {
    readIos += perf.istats[0][w].readIos;
    readCount += perf.mstats[0][w].readCount;
    readHits += perf.mstats[0][w].readHits;
    readMisses += perf.mstats[0][w].readMisses;
  }
  // Reads flowed end to end through the async + cache path.
  EXPECT_GT(readIos, 0u);
  // The breakdown partitions every counted read.
  EXPECT_EQ(readCount, readHits + readMisses);
  // Reuse over the cached key space produces hits on the async path...
  EXPECT_GT(readHits, 0u);
  // ...and the cache short-circuits the worker: only cold first-touches are
  // offloaded (a handful, bounded by the key space and the in-flight depth that
  // can race the same id before the cache fills), so hits vastly outnumber the
  // misses that actually reached a worker.
  EXPECT_GT(readHits, readMisses);

  fs::remove_all(dir);
}

// RocksDb backend, read path against an empty (unpopulated) chunk map: every
// lookup misses, so the reactor does no I/O but still sets up and tears down
// cleanly without crashing or hanging.
TEST(IoUringEngineTest, Run_RocksDbReadPath_EmptyDb_NoCrash) {
  const std::string dir = uniqueDir("rocks_empty");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 2;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoCoreReactor reactor(
      makeReactorConfig(
          dir,
          queueDepth,
          /*readRatio=*/1.0,
          /*iops=*/0.0,
          MetadataBackend::RocksDb),
      &perf);

  const uint64_t now = current_nano();
  EXPECT_NO_THROW(
      reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 50'000'000ull));

  uint64_t totalReadIos = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalReadIos += perf.istats[0][w].readIos;
  }
  EXPECT_EQ(totalReadIos, 0u);

  for (const auto& slot : reactor.slots()) {
    EXPECT_EQ(slot.state, SlotState::Idle);
  }

  fs::remove_all(dir);
}

// RocksDb backend, ASYNC read path: with a request/result queue wired in, the
// reactor offloads each chunk-map lookup to a MetadataWorker instead of looking
// up inline. Over a pre-populated map every read still resolves to a stored
// location and reads exactly its length, and one metadata read is recorded per
// disk read.
TEST(IoUringEngineTest, Run_RocksDbAsyncReadPath_OffloadsLookups) {
  const std::string dir = uniqueDir("rocks_async_read");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);
  populateChunkMap(dir, /*numChunks=*/8);

  const int queueDepth = 4;
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);

  // One shared request queue + one result queue for the single reactor, drained
  // by a worker thread (capacities cover one outstanding lookup per slot).
  LookupRequestQueue requestQueue(queueDepth);
  LookupResultQueue resultQueue(queueDepth);
  MetadataWorker worker(&requestQueue, {&resultQueue}, /*maxBatch=*/queueDepth);
  std::thread workerThread([&worker] { worker.run(); });

  {
    ReactorConfig rc = makeReactorConfig(
        dir,
        queueDepth,
        /*readRatio=*/1.0,
        /*iops=*/0.0,
        MetadataBackend::RocksDb);
    rc.requestQueue = &requestQueue;
    rc.resultQueue = &resultQueue;
    IoCoreReactor reactor(std::move(rc), &perf);
    const uint64_t now = current_nano();
    reactor.run(/*warmupEndNs=*/now, /*deadlineNs=*/now + 200'000'000ull);
  }
  worker.stop();
  workerThread.join();

  uint64_t totalReadIos = 0;
  uint64_t totalReadBytes = 0;
  uint64_t totalMetaReads = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalReadIos += perf.istats[0][w].readIos;
    totalReadBytes += perf.istats[0][w].readBytes;
    totalMetaReads += perf.mstats[0][w].readCount;
  }
  EXPECT_GT(totalReadIos, 0u);
  EXPECT_EQ(totalReadBytes, totalReadIos * static_cast<uint64_t>(kMinIoSize));
  // Every chunk is populated, so each offloaded lookup hits -> exactly one
  // metadata read per disk read.
  EXPECT_EQ(totalMetaReads, totalReadIos);

  fs::remove_all(dir);
}

// RocksDb backend through the full engine with QMetadata workers: IoUringEngine
// builds the shared lookup queues, spawns the worker pool, offloads reads, and
// tears everything down cleanly. Over a pre-populated map reads flow and the
// metadata-read path is exercised end to end.
TEST(IoUringEngineTest, Run_RocksDbAsyncEngine_RunsWorkersAndTearsDown) {
  const std::string dir = uniqueDir("rocks_async_engine");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);
  populateChunkMap(dir, /*numChunks=*/16);

  const int queueDepth = 4;
  HalcyonPools pools =
      buildPools(PoolConfig{.qioThreads = 1, .qmetaThreads = 2});
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoSizeRange fourK;
  fourK.upperBound = kMinIoSize;
  fourK.percentage = 100;

  IoUringEngine::Config config;
  config.mountPoints = {dir};
  config.warmupSec = 0;
  config.runtimeSec = 1;
  config.threadsPerDisk = queueDepth;
  config.doDirectIo = false;
  config.readRatio = 1.0;
  config.readSizes = {fourK};
  config.writeSizes = {fourK};
  config.backend = MetadataBackend::RocksDb;
  config.qmetaThreads = 2;
  config.perfStats = &perf;

  IoUringEngine engine(pools, std::move(config));
  EXPECT_NO_THROW(engine.run());

  uint64_t totalReadIos = 0;
  uint64_t totalMetaReads = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalReadIos += perf.istats[0][w].readIos;
    totalMetaReads += perf.mstats[0][w].readCount;
  }
  EXPECT_GT(totalReadIos, 0u);
  EXPECT_EQ(totalMetaReads, totalReadIos);

  fs::remove_all(dir);
}

// RocksDb backend through the full engine with QFlush workers, write path
// (readRatio=0): IoUringEngine builds the shared flush queue, spawns the flush
// pool, offloads each completed write's chunk-map put, drains every flush, and
// tears down cleanly. The put cost moves off the reactor (no inline
// writeNanos), and the persisted chunk map holds the written key range.
TEST(IoUringEngineTest, Run_RocksDbAsyncEngine_FlushesWritesOffThread) {
  const std::string dir = uniqueDir("rocks_async_flush");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 4;
  HalcyonPools pools =
      buildPools(PoolConfig{.qioThreads = 1, .qflushThreads = 2});
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoSizeRange fourK;
  fourK.upperBound = kMinIoSize;
  fourK.percentage = 100;

  IoUringEngine::Config config;
  config.mountPoints = {dir};
  config.warmupSec = 0;
  config.runtimeSec = 1;
  config.threadsPerDisk = queueDepth;
  config.doDirectIo = false;
  config.readRatio = 0.0;
  config.readSizes = {fourK};
  config.writeSizes = {fourK};
  config.backend = MetadataBackend::RocksDb;
  config.qflushThreads = 2;
  config.perfStats = &perf;

  IoUringEngine engine(pools, std::move(config));
  EXPECT_NO_THROW(engine.run());

  uint64_t totalWriteIos = 0;
  uint64_t totalMetaWrites = 0;
  uint64_t totalMetaWriteNanos = 0;
  for (int w = 0; w < queueDepth; ++w) {
    totalWriteIos += perf.istats[0][w].writeIos;
    totalMetaWrites += perf.mstats[0][w].writeCount;
    totalMetaWriteNanos += perf.mstats[0][w].writeNanos;
  }
  EXPECT_GT(totalWriteIos, 0u);
  // Every completed write is counted for throughput, but its put runs on the
  // QFlush pool -- so no inline writeNanos is recorded on the reactor.
  EXPECT_EQ(totalMetaWrites, totalWriteIos);
  EXPECT_EQ(totalMetaWriteNanos, 0u);

  // All flushes drained before teardown destroyed each mapper: the persisted
  // chunk map holds a non-empty key range and id 0 maps to a valid 4K extent.
  HalcyonChunkMapper mapper = openChunkMap(dir);
  ASSERT_GT(mapper.maxChunkId(), 0u);
  ChunkLocation loc;
  ASSERT_TRUE(mapper.lookup(0, loc));
  EXPECT_EQ(loc.length, static_cast<uint32_t>(kMinIoSize));
  EXPECT_GE(loc.offset, static_cast<uint64_t>(kChunkHeaderSize));

  fs::remove_all(dir);
}

// RocksDb backend through the full engine with the read cache enabled via
// IoUringEngine::Config: confirms config.cacheCapacity is propagated to each
// reactor (the 1f plumbing). Over a small pre-populated key space that fits in
// the cache, reads are read-through, so the cache produces hits and the
// counters satisfy readCount == readHits + readMisses.
TEST(IoUringEngineTest, Run_RocksDbEngine_CacheCapacityPropagatesFromConfig) {
  const std::string dir = uniqueDir("rocks_engine_cache");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);
  populateChunkMap(dir, /*numChunks=*/8);

  const int queueDepth = 4;
  HalcyonPools pools = buildPools(PoolConfig{.qioThreads = 1});
  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  IoSizeRange fourK;
  fourK.upperBound = kMinIoSize;
  fourK.percentage = 100;

  IoUringEngine::Config config;
  config.mountPoints = {dir};
  config.warmupSec = 0;
  config.runtimeSec = 1;
  config.threadsPerDisk = queueDepth;
  config.doDirectIo = false;
  config.readRatio = 1.0;
  config.readSizes = {fourK};
  config.writeSizes = {fourK};
  config.backend = MetadataBackend::RocksDb;
  config.cacheCapacity = 64; // fits the 8-chunk key space
  config.perfStats = &perf;

  IoUringEngine engine(pools, std::move(config));
  EXPECT_NO_THROW(engine.run());

  uint64_t readCount = 0;
  uint64_t readHits = 0;
  uint64_t readMisses = 0;
  for (int w = 0; w < queueDepth; ++w) {
    readCount += perf.mstats[0][w].readCount;
    readHits += perf.mstats[0][w].readHits;
    readMisses += perf.mstats[0][w].readMisses;
  }
  // The cache was built from config.cacheCapacity (propagated to the reactors),
  // so repeated reads over the tiny key space hit, and the breakdown is exact.
  EXPECT_GT(readCount, 0u);
  EXPECT_GT(readHits, 0u);
  EXPECT_EQ(readHits + readMisses, readCount);

  fs::remove_all(dir);
}

// IoBufferPool hands out every buffer once (distinct + aligned), reports
// exhaustion with nullptr rather than blocking, and makes a released buffer
// available again. This is the invariant the reactor relies on for backpressure
// (empty pool -> acquire() == nullptr -> the slot is retried, not stalled).
TEST(IoUringEngineTest, IoBufferPool_AcquireReleaseAndExhaustion) {
  const size_t count = 3;
  IoBufferPool pool(count, kMaxFileSize);

  std::vector<uint8_t*> bufs;
  for (size_t i = 0; i < count; ++i) {
    uint8_t* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(
        reinterpret_cast<uintptr_t>(b) % kMinIoSize, 0u); // O_DIRECT align
    bufs.push_back(b);
  }
  // The pool handed out `count` distinct buffers.
  std::sort(bufs.begin(), bufs.end());
  EXPECT_EQ(std::unique(bufs.begin(), bufs.end()), bufs.end());
  // Exhausted: acquire() reports empty via nullptr instead of blocking.
  EXPECT_EQ(pool.acquire(), nullptr);

  // A returned buffer becomes acquirable again; releasing null is a no-op.
  uint8_t* returned = bufs.back();
  pool.release(returned);
  pool.release(nullptr);
  EXPECT_EQ(pool.acquire(), returned);
  EXPECT_EQ(pool.acquire(), nullptr);
}

// End-to-end pooled send path (PairRole::SendDiskReads shape): with a
// bufferPool and sendQueue wired, completed reads hand their buffer to the
// network layer by moving ownership (no copy) and the reactor arms the next op
// from a fresh pool buffer. A background consumer drains the queue -- each
// SendItem's deleter returns its buffer to the pool -- mimicking the
// NetworkWorker. Afterwards every buffer is back in the pool (no leak, no
// double-free) and all slots are Idle with no borrowed buffer.
TEST(IoUringEngineTest, Run_PooledSendPath_HandsOffAndReturnsBuffers) {
  const std::string dir = uniqueDir("pool_send");
  makeFileset(dir, /*dirs=*/1, /*files=*/1, /*fileSize=*/kMaxFileSize);

  const int queueDepth = 4;
  const size_t sendCap = 16;
  const size_t poolBuffers = static_cast<size_t>(queueDepth) + sendCap;
  IoBufferPool pool(poolBuffers, kMaxFileSize);
  SendDataQueue sendQueue(sendCap);

  PerfStats perf(/*operators=*/1, /*workers=*/queueDepth);
  ReactorConfig rc = makeReactorConfig(dir, queueDepth, /*readRatio=*/1.0);
  rc.sendQueue = &sendQueue;
  rc.bufferPool = &pool;
  IoCoreReactor reactor(rc, &perf);

  // Consumer: drains SendItems (whose deleter returns the buffer to the pool)
  // so the reactor's bounded blockingWrite never stalls, like NetworkWorker.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> forwarded{0};
  std::thread drainer([&] {
    SendItem item;
    while (!stop.load(std::memory_order_relaxed)) {
      while (sendQueue.readIfNotEmpty(item)) {
        ++forwarded;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    while (sendQueue.readIfNotEmpty(item)) {
      ++forwarded;
    }
  });

  const uint64_t now = current_nano();
  reactor.run(/*warmupEndNs=*/now,
              /*deadlineNs=*/now + 200'000'000ull); // 200ms
  stop.store(true);
  drainer.join();

  // Reads actually flowed through the pooled handoff to the network layer.
  EXPECT_GT(forwarded.load(), 0u);

  // Slots drained to Idle and relinquished their buffers (pooled mode holds no
  // per-slot buffer between ops).
  for (const auto& slot : reactor.slots()) {
    EXPECT_EQ(slot.state, SlotState::Idle);
    EXPECT_EQ(slot.buffer, nullptr);
  }

  // Every buffer is back in the pool: exactly poolBuffers can be re-acquired
  // and no more. This fails if any buffer leaked (handoff not returned) or was
  // double-returned (would over-fill the free list).
  std::vector<uint8_t*> reacquired;
  for (size_t i = 0; i < poolBuffers; ++i) {
    uint8_t* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    reacquired.push_back(b);
  }
  EXPECT_EQ(pool.acquire(), nullptr);
  for (uint8_t* b : reacquired) {
    pool.release(b);
  }

  fs::remove_all(dir);
}

} // namespace facebook::halcyon
