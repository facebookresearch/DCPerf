// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "IoUringEngine.h"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <liburing.h>

#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <folly/FollyMemcpy.h>
#include <folly/Random.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <rocksdb/cache.h>

#include "ChunkMetadataCache.h"
#include "Common.h"
#include "FlushWorker.h"
#include "HalcyonChunkMapper.h"
#include "MetadataWorker.h"
#include "PerfStats.h"

DEFINE_int32(
    rocks_block_cache_mb,
    640,
    "RocksDB block cache size per HalcyonChunkMapper instance (one per mount) "
    "in MiB. Default 640 aggregates to 10 GiB across 16 mounts, matching "
    "Hypernode's sharedBlockCacheSizeMiB=10240 (see server/config/T8*.jsonc).");

namespace facebook::halcyon {

namespace {
// Max chunk-map lookups a QMetadata worker coalesces into one batched MultiGet.
constexpr size_t kLookupBatch = 32;
// Max chunk-map writes a QFlush worker coalesces into one putBatch + syncWal.
constexpr size_t kFlushBatch = 32;
} // namespace

IoBufferPool::IoBufferPool(size_t count, size_t bufferSize)
    : free_(std::max<size_t>(count, 1)) {
  all_.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    void* buffer = nullptr;
    if (posix_memalign(&buffer, kMinIoSize, bufferSize) != 0 ||
        buffer == nullptr) {
      for (uint8_t* b : all_) {
        free(b);
      }
      all_.clear();
      throw std::bad_alloc();
    }
    all_.push_back(static_cast<uint8_t*>(buffer));
    // capacity == count, so this write never blocks.
    free_.blockingWrite(static_cast<uint8_t*>(buffer));
  }
}

IoBufferPool::~IoBufferPool() {
  for (uint8_t* b : all_) {
    free(b);
  }
}

uint8_t* IoBufferPool::acquire() {
  uint8_t* buf = nullptr;
  return free_.readIfNotEmpty(buf) ? buf : nullptr;
}

void IoBufferPool::release(uint8_t* buffer) {
  if (buffer == nullptr) {
    return;
  }
  // Never blocks: capacity == total buffer count and we only ever return a
  // buffer that was previously acquired, so the free list can't overflow.
  free_.blockingWrite(buffer);
}

IoCoreReactor::IoCoreReactor(ReactorConfig config, PerfStats* perfStats)
    : config_(std::move(config)), perfStats_(perfStats) {}

IoCoreReactor::~IoCoreReactor() {
  // In pooled mode slot buffers are borrowed from the shared IoBufferPool
  // (returned on completion / handed off to the network layer), so the pool
  // owns and frees them; only free self-allocated buffers here.
  if (config_.bufferPool == nullptr) {
    for (auto& slot : slots_) {
      free(slot.buffer);
      slot.buffer = nullptr;
    }
  }
  for (const int fd : fds_) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
}

void IoCoreReactor::readManifest() {
  const std::string fname = config_.baseDir + "/manifest";
  if (!fileExistsAndIsReadable(fname)) {
    throw std::runtime_error(
        fmt::format("No manifest file in {}", config_.baseDir));
  }
  std::string manifest;
  if (!folly::readFile(fname.c_str(), manifest)) {
    throw std::runtime_error(
        fmt::format("Could not read manifest in {}", config_.baseDir));
  }
  int dirs = 0;
  int files = 0;
  if (!folly::split(' ', manifest, dirs, files)) {
    throw std::runtime_error(
        fmt::format("Error parsing manifest in {}", config_.baseDir));
  }
  dirs_ = dirs;
  files_ = files;
}

void IoCoreReactor::openFiles() {
  readManifest();

  FileSelector fs;
  fs.dirs = dirs_;
  fs.files = files_;

  const int numFiles = dirs_ * files_;
  // Guard against an empty fileset (e.g. a manifest of "0 0"): with no fds, the
  // read path would pass 0 as the exclusive upper bound to rand32 (UB) and then
  // index an empty fds_. Fail fast here instead, before any I/O is armed.
  if (numFiles <= 0) {
    throw std::runtime_error(
        fmt::format(
            "Empty fileset in {} (manifest reports {} dirs x {} files)",
            config_.baseDir,
            dirs_,
            files_));
  }
  fds_.reserve(numFiles);

  // O_SYNC (direct I/O only) makes writes durable on completion, matching the
  // legacy write path's O_WRONLY|O_DIRECT|O_SYNC, so no per-op fsync SQE is
  // needed. Reads are unaffected by O_SYNC.
  int flags = O_RDWR;
  if (config_.doDirectIo) {
    flags |= O_DIRECT | O_SYNC;
  }

  for (int i = 0; i < numFiles; ++i) {
    const std::string path = config_.baseDir + fs.partialPath(i);
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
      const int err = errno;
      throw std::runtime_error(
          fmt::format("Failed to open {} (errno {})", path, err));
    }
    fds_.push_back(fd);
  }
}

void IoCoreReactor::allocateSlots() {
  // resize default-constructs OpSlots (buffer == nullptr, state == Idle); if
  // posix_memalign throws partway, the destructor frees whatever was already
  // allocated.
  slots_.resize(config_.queueDepth);
  if (config_.bufferPool != nullptr) {
    // Pooled mode: buffers are acquired per-op from the shared pool, not owned
    // per-slot. Leave slot.buffer null -- a slot pulls a buffer when armed and
    // returns/hands it off on completion.
    return;
  }
  for (auto& slot : slots_) {
    void* buffer = nullptr;
    if (posix_memalign(&buffer, kMinIoSize, kMaxFileSize) != 0 ||
        buffer == nullptr) {
      throw std::bad_alloc();
    }
    slot.buffer = buffer;
    slot.state = SlotState::Idle;
  }
}

bool IoCoreReactor::submitRead(io_uring* ring, int slotIndex) {
  OpSlot& slot = slots_[slotIndex];

  uint32_t fileIndex = 0;
  int ioSize = 0;
  uint64_t offset = 0;
  ChunkId chunkId = 0;

  // Select the target before fetching an SQE: a RocksDb lookup may miss (e.g.
  // unpopulated DB), and we must not consume an SQE we won't fill.
  if (config_.backend == MetadataBackend::RocksDb) {
    // Metadata in the hot path: pick an existing chunk id and look up its
    // physical location in RocksDB before issuing the disk read.
    const ChunkId bound =
        std::max<ChunkId>(nextChunkId_, mapper_->maxChunkId());
    const ChunkId id = HalcyonChunkMapper::randomLocalKey(
        bound, config_.readLocality, config_.localityWindow);
    ChunkLocation loc;
    // Resolve the location, preferring the per-reactor LRU cache when enabled:
    // a cache hit skips the DB lookup (only the probe is charged), a cache miss
    // pays the full DB read and populates the cache. Time the whole resolution
    // inline (like the checksum) so the metadata read cost is attributable; the
    // cost is incurred whether or not the key is found, so record before
    // handling a miss; gate on warmup to match the I/O stats.
    const uint64_t mdStart = current_nano();
    const bool cacheHit = cache_ && cache_->lookup(id, loc);
    bool hit = cacheHit;
    if (!cacheHit) {
      hit = mapper_->lookup(id, loc);
      if (hit && cache_) {
        cache_->insert(id, loc);
      }
    }
    const uint64_t mdNs = current_nano() - mdStart;
    if (mdStart >= warmupEndNs_) {
      MetadataStats& mst = perfStats_->mstats[config_.operatorId][slotIndex];
      mst.readCount++;
      mst.readNanos += mdNs;
      // Breakdown only meaningful with a cache; keeps readCount == hits+misses.
      if (cache_) {
        if (cacheHit) {
          mst.readHits++;
        } else {
          mst.readMisses++;
        }
      }
    }
    if (!hit) {
      return false; // miss: nothing written yet for this id; skip this slot
    }
    fileIndex = loc.fileIndex;
    offset = loc.offset;
    ioSize = static_cast<int>(loc.length);
    chunkId = id;
  } else {
    fileIndex = folly::Random::rand32(fds_.size());
    ioSize = getIoSize(folly::Random::randDouble01(), &config_.readSizes);
    offset = randomOffset(ioSize);
  }

  // Pooled mode: acquire the read buffer before taking an SQE so the buffer
  // can later be moved into a SendItem with no copy. An empty pool means the
  // send pipeline is backed up -- leave the slot Idle and retry next iteration.
  void* buffer = slot.buffer;
  if (config_.bufferPool != nullptr) {
    buffer = config_.bufferPool->acquire();
    if (buffer == nullptr) {
      return false;
    }
  }

  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    if (config_.bufferPool != nullptr) {
      config_.bufferPool->release(static_cast<uint8_t*>(buffer));
    }
    return false; // ring is full; reap completions before preparing more
  }

  const int fd = fds_[fileIndex];
  slot.buffer = buffer;
  io_uring_prep_read(sqe, fd, slot.buffer, ioSize, offset);
  io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(slotIndex));

  slot.fd = fd;
  slot.fileIndex = fileIndex;
  slot.offset = offset;
  slot.ioSize = ioSize;
  slot.chunkId = chunkId;
  slot.isWrite = false;
  slot.ioStartNs = current_nano();
  slot.opStartNs = slot.ioStartNs; // synchronous path: op start == disk submit
  slot.state = SlotState::IoInflight;
  return true;
}

bool IoCoreReactor::submitWrite(io_uring* ring, int slotIndex) {
  OpSlot& slot = slots_[slotIndex];

  // Pooled mode: acquire this write's buffer from the shared pool (its content
  // is irrelevant benchmark bytes), returned on completion. Empty pool ->
  // backpressure, retry next iteration.
  void* buffer = slot.buffer;
  if (config_.bufferPool != nullptr) {
    buffer = config_.bufferPool->acquire();
    if (buffer == nullptr) {
      return false;
    }
  }

  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    if (config_.bufferPool != nullptr) {
      config_.bufferPool->release(static_cast<uint8_t*>(buffer));
    }
    return false; // ring is full; reap completions before preparing more
  }
  slot.buffer = buffer;

  const uint32_t fileIndex =
      folly::Random::rand32(static_cast<uint32_t>(fds_.size()));
  const int fd = fds_[fileIndex];
  const int ioSize = getPureIoSize(
      getIoSize(folly::Random::randDouble01(), &config_.writeSizes));

  uint64_t offset = 0;
  ChunkId chunkId = 0;
  if (config_.backend == MetadataBackend::RocksDb) {
    // Bump-allocate an aligned, non-overlapping offset within the file so the
    // read path lands on written data; wrap to the chunk body start when the
    // file fills. getPureIoSize keeps ioSize a 4K multiple, so offsets stay
    // O_DIRECT-aligned. The mapping is persisted on completion, after the data
    // write is durable.
    offset = nextOffset_[fileIndex];
    if (offset + static_cast<uint64_t>(ioSize) >
        static_cast<uint64_t>(kMaxFileSize)) {
      offset = kChunkHeaderSize;
    }
    nextOffset_[fileIndex] = offset + static_cast<uint64_t>(ioSize);
    chunkId = nextChunkId_++;
  } else {
    // Legacy write path: write the on-disk (pure) size at offset 0.
    offset = 0;
  }

  io_uring_prep_write(sqe, fd, slot.buffer, ioSize, offset);
  io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(slotIndex));

  slot.fd = fd;
  slot.fileIndex = fileIndex;
  slot.offset = offset;
  slot.ioSize = ioSize;
  slot.chunkId = chunkId;
  slot.isWrite = true;
  slot.ioStartNs = current_nano();
  slot.opStartNs = slot.ioStartNs; // synchronous path: op start == disk submit
  slot.state = SlotState::IoInflight;
  return true;
}

bool IoCoreReactor::submitRecvWrite(
    io_uring* ring,
    int slotIndex,
    RecvItem&& item) {
  // Fully-coupled write path (PairRole::SendDiskBoth). Same on-disk layout
  // as submitWrite -- pick a random file, bump-allocate an offset, persist
  // (chunk_id -> location) after completion -- but the payload came from
  // the partner over the network instead of being generated from RAM.
  //
  // Two paths:
  //   Fast: HN-style opportunistic aliasing (matches
  //   hypernode/data/dpc/IoCore.cpp:591-610). If the incoming Thrift
  //   IOBuf is unchained, its base pointer is word-aligned, and its
  //   length is a kMinIoSize multiple, we submit io_uring_prep_writev
  //   with a single iovec pointing at the IOBuf's own memory. No
  //   alignment memcpy. The IOBuf is parked on the OpSlot so its
  //   memory stays live until the kernel completion.
  //   Fallback: pool-acquire + Cursor::pull. Used when the IOBuf is
  //   chained, misaligned, or the pool is empty. This is what HN's
  //   transferIOBufData does on its !skipDataTransfer branch too.
  OpSlot& slot = slots_[slotIndex];
  const uint32_t fileIndex =
      folly::Random::rand32(static_cast<uint32_t>(fds_.size()));
  const int fd = fds_[fileIndex];
  if (item.data == nullptr) {
    LOG_FIRST_N(ERROR, 5) << "submitRecvWrite: null IOBuf";
    return true; // drop this recv; nothing acquired
  }
  // Received payload size drives the disk write size (matches network op size).
  // Truncate to the slot buffer capacity if the peer sent an oversized payload.
  const size_t totalSize = item.data->computeChainDataLength();
  const size_t recvSize =
      std::min<size_t>(totalSize, static_cast<size_t>(kMaxFileSize));
  // Round DOWN to O_DIRECT sector alignment. Cannot use getPureIoSize here --
  // that helper adds the on-disk chunk footer overhead (~0.8%) meant for
  // locally-generated writes; applied to received payload it produces
  // ioSize > totalSize and the subsequent cursor.pull would underflow the
  // IOBuf chain.
  const int ioSize =
      static_cast<int>(recvSize & ~(static_cast<size_t>(kMinIoSize) - 1));
  if (ioSize <= 0) {
    return true; // drop this recv; nothing acquired
  }

  // Zero-copy fast-path predicate (matches HN's isWordAlignedBuf + LBA-length
  // check). We additionally require the single IOBuf to cover ioSize bytes so
  // we can point one iovec at it directly. Word alignment (4 B) is
  // guaranteed by any malloc/jemalloc allocation, so the payload check is the
  // real gate -- Thrift usually delivers unchained 1 MiB payloads that
  // satisfy this.
  const bool unchained =
      item.data->next() == item.data.get() && !item.data->isChained();
  const uintptr_t base = reinterpret_cast<uintptr_t>(item.data->data());
  const bool wordAligned =
      (base & (static_cast<uintptr_t>(sizeof(uint32_t)) - 1)) == 0;
  const bool coversIoSize =
      unchained && item.data->length() >= static_cast<size_t>(ioSize);
  const bool zeroCopyOk = unchained && wordAligned && coversIoSize;

  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    return false; // ring is full; retry next iteration (nothing acquired)
  }

  uint64_t offset = 0;
  ChunkId chunkId = 0;
  if (config_.backend == MetadataBackend::RocksDb) {
    offset = nextOffset_[fileIndex];
    if (offset + static_cast<uint64_t>(ioSize) >
        static_cast<uint64_t>(kMaxFileSize)) {
      offset = kChunkHeaderSize;
    }
    nextOffset_[fileIndex] = offset + static_cast<uint64_t>(ioSize);
    chunkId = nextChunkId_++;
  }

  if (zeroCopyOk) {
    // Fast path: alias the IOBuf into a single-iovec writev. No memcpy.
    // Park the IOBuf on the slot so its memory outlives the kernel completion;
    // handleCompletion resets it.
    slot.buffer = nullptr;
    slot.recvIOBuf = std::move(item.data);
    struct iovec iov = {
        const_cast<uint8_t*>(slot.recvIOBuf->data()),
        static_cast<size_t>(ioSize)};
    io_uring_prep_writev(sqe, fd, &iov, 1, offset);
    ++zeroCopyRecvWrites_;
  } else {
    // Fallback: pool-acquire + Cursor::pull, one alignment memcpy.
    void* buffer = slot.buffer;
    if (config_.bufferPool != nullptr) {
      buffer = config_.bufferPool->acquire();
      if (buffer == nullptr) {
        // Return the SQE (we haven't wired data yet). io_uring will treat
        // this SQE as unused on the next submit -- drop the recv.
        return true;
      }
    }
    if (buffer == nullptr) {
      LOG_FIRST_N(ERROR, 5) << "submitRecvWrite: null slot buffer";
      return true;
    }
    slot.buffer = buffer;
    folly::io::Cursor cursor(item.data.get());
    cursor.pull(slot.buffer, static_cast<size_t>(ioSize));
    io_uring_prep_write(sqe, fd, slot.buffer, ioSize, offset);
    ++fallbackRecvWrites_;
  }
  io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(slotIndex));

  slot.fd = fd;
  slot.fileIndex = fileIndex;
  slot.offset = offset;
  slot.ioSize = ioSize;
  slot.chunkId = chunkId;
  slot.isWrite = true;
  slot.ioStartNs = current_nano();
  slot.opStartNs = slot.ioStartNs;
  slot.state = SlotState::IoInflight;
  return true;
}

void IoCoreReactor::enqueueLookup(int slotIndex, uint64_t now) {
  // requestQueue is set whenever async lookups are enabled (see run()), so this
  // is never null here; the guard documents the invariant and satisfies the
  // null-safety analyzer.
  auto* const requestQueue = config_.requestQueue;
  if (requestQueue == nullptr) {
    return;
  }
  OpSlot& slot = slots_[slotIndex];
  // Pick a chunk id (same selection as the inline path, with the same injected
  // read locality).
  const ChunkId bound = std::max<ChunkId>(nextChunkId_, mapper_->maxChunkId());
  const ChunkId id = HalcyonChunkMapper::randomLocalKey(
      bound, config_.readLocality, config_.localityWindow);
  slot.chunkId = id;
  slot.opStartNs =
      now; // charge pacing from enqueue, including the metadata wait

  // Probe the per-reactor cache on the reactor thread first (the QMetadata
  // workers never touch it, so it stays lock-free). A hit resolves the location
  // here and skips the worker round-trip entirely -- it goes straight onto the
  // pending-read queue, like a resolved lookup. Only a miss is handed to the
  // pool (and counted in drainLookupResults when the result returns), keeping
  // readCount == readHits + readMisses.
  if (cache_) {
    ChunkLocation loc;
    const uint64_t mdStart = current_nano();
    if (cache_->lookup(id, loc)) {
      // Hit: count it here and charge only the probe time. A miss is NOT
      // counted here -- it is counted in drainLookupResults when the worker's
      // result returns, so readCount stays == readHits + readMisses (no double
      // count).
      if (now >= warmupEndNs_) {
        MetadataStats& mst = perfStats_->mstats[config_.operatorId][slotIndex];
        mst.readCount++;
        mst.readNanos += current_nano() - mdStart;
        mst.readHits++;
      }
      // Resolved without the pool; armed later by submitPendingReads. Parked in
      // MetadataInflight (not Idle) so the main loop won't re-arm it meanwhile.
      slot.state = SlotState::MetadataInflight;
      pendingReads_.push_back(MappedRead{slotIndex, loc});
      return;
    }
  }

  // Cache miss (or cache disabled): hand the lookup to the shared QMetadata
  // pool; the worker posts the result back to this reactor's result queue,
  // which run() drains.
  LookupRequest req;
  req.mapper = mapper_.get();
  req.id = id;
  req.reactorId = config_.operatorId;
  req.slotIndex = slotIndex;
  // Capacity is sized to the total slot count, so a slot (which holds at most
  // one outstanding lookup) can always enqueue without blocking.
  requestQueue->blockingWrite(req);

  slot.state = SlotState::MetadataInflight;
  ++metadataInflight_;
}

void IoCoreReactor::drainLookupResults(uint64_t now) {
  // resultQueue is set whenever async lookups are enabled (see run()), so this
  // is never null here; the guard documents the invariant and satisfies the
  // null-safety analyzer.
  auto* const resultQueue = config_.resultQueue;
  if (resultQueue == nullptr) {
    return;
  }
  LookupResult res;
  while (resultQueue->try_dequeue(res)) {
    --metadataInflight_;
    OpSlot& slot = slots_[res.slotIndex];
    // The metadata-read cost is incurred whether or not the key was found; gate
    // the stat on warmup like the inline path did.
    if (now >= warmupEndNs_) {
      MetadataStats& mst =
          perfStats_->mstats[config_.operatorId][res.slotIndex];
      mst.readCount++;
      mst.readNanos += res.lookupNanos;
      // Everything routed to a worker was a cache miss (the reactor probes the
      // cache before enqueuing). Keeps readCount == readHits + readMisses.
      mst.readMisses += cache_ ? 1 : 0;
    }
    if (!res.found) {
      slot.nextSubmitNs = slot.opStartNs + targetSpacingNs_;
      slot.state = SlotState::Idle; // miss: nothing to read; free the slot
      continue;
    }
    // Read-through populate: cache the resolved location (keyed by the id
    // stored on the slot at enqueue) so a later read of this id hits in-cache.
    if (cache_) {
      cache_->insert(slot.chunkId, res.loc);
    }
    pendingReads_.push_back(MappedRead{res.slotIndex, res.loc});
  }
}

bool IoCoreReactor::submitMappedRead(
    io_uring* ring,
    int slotIndex,
    const ChunkLocation& loc) {
  OpSlot& slot = slots_[slotIndex];

  // Pooled mode: acquire the read buffer up front so it can be handed off with
  // no copy on completion. Empty pool -> backpressure; the caller keeps this
  // read pending and retries next iteration.
  void* buffer = slot.buffer;
  if (config_.bufferPool != nullptr) {
    buffer = config_.bufferPool->acquire();
    if (buffer == nullptr) {
      return false;
    }
  }

  io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == nullptr) {
    if (config_.bufferPool != nullptr) {
      config_.bufferPool->release(static_cast<uint8_t*>(buffer));
    }
    return false; // ring is full; reap completions before preparing more
  }

  const int fd = fds_[loc.fileIndex];
  const int ioSize = static_cast<int>(loc.length);
  slot.buffer = buffer;
  io_uring_prep_read(sqe, fd, slot.buffer, ioSize, loc.offset);
  io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(slotIndex));

  slot.fd = fd;
  slot.fileIndex = loc.fileIndex;
  slot.offset = loc.offset;
  slot.ioSize = ioSize;
  slot.isWrite = false;
  slot.ioStartNs =
      current_nano(); // I/O latency starts now; opStartNs unchanged
  slot.state = SlotState::IoInflight;
  return true;
}

int IoCoreReactor::submitPendingReads(io_uring* ring) {
  int prepared = 0;
  while (!pendingReads_.empty()) {
    const MappedRead pr = pendingReads_.back();
    if (!submitMappedRead(ring, pr.slotIndex, pr.loc)) {
      break; // ring full; the rest stay pending for the next iteration
    }
    pendingReads_.pop_back();
    ++prepared;
  }
  return prepared;
}

void IoCoreReactor::handleCompletion(io_uring_cqe* cqe, uint64_t warmupEndNs) {
  const auto slotIndex = static_cast<size_t>(io_uring_cqe_get_data64(cqe));
  OpSlot& slot = slots_[slotIndex];
  const int res = cqe->res;
  const uint64_t ioEnd = current_nano();

  // Next op on this slot may not start before targetSpacingNs after this op's
  // start, which spreads the configured iops evenly across slots (0 == ASAP).
  // opStartNs == ioStartNs on the synchronous path; for an async RocksDb read
  // it is the lookup-enqueue time, so a read's metadata wait counts toward its
  // pacing interval.
  slot.nextSubmitNs = slot.opStartNs + targetSpacingNs_;

  if (res < 0) {
    LOG(ERROR) << fmt::format(
        "io_uring {} failed (fd {}, offset {}, size {}): {}",
        slot.isWrite ? "write" : "read",
        slot.fd,
        slot.offset,
        slot.ioSize,
        res);
    if (config_.bufferPool != nullptr) {
      config_.bufferPool->release(static_cast<uint8_t*>(slot.buffer));
      slot.buffer = nullptr;
    }
    slot.state = SlotState::Idle;
    return;
  }

  const uint64_t latUsec = (ioEnd - slot.ioStartNs) / 1000;
  const bool afterWarmup = ioEnd >= warmupEndNs;

  // Inline checksum while the buffer is still cache-hot (Option B). The CPU is
  // spent regardless of warmup; only the stat recording is gated. Timed in
  // software so the QIOThread checksum fraction stays measurable without a
  // separate QCPU pool.
  //
  // Checksum only the bytes the kernel actually read (`res`), not the requested
  // size: a short read leaves [res, ioSize) holding stale/uninitialized buffer
  // bytes, so checksumming them would be meaningless work and would inflate the
  // cstats readNanos/readCount relative to the data really returned.
  // Skip checksum + compress when slot.buffer is null: this is the
  // SendDiskBoth zero-copy recv-write fast path (submitRecvWrite aliased the
  // IOBuf's own memory into an iovec). There is no reactor-owned scratch
  // buffer to checksum, and pulling into the IOBuf here just to emulate the
  // per-op CPU cost would erase the memBW savings the fast path just
  // recovered. HN's `hypernode.aligned_parser_put` fast-path skips the same
  // per-op memcpy for the same reason.
  const uint64_t csStart = current_nano();
  const int chksums = slot.buffer != nullptr
      ? doChecksum(static_cast<uint8_t*>(slot.buffer), res)
      : 0;
  const uint64_t csNs = current_nano() - csStart;

  IoStats& ist = perfStats_->istats[config_.operatorId][slotIndex];
  ChecksumStats& cst = perfStats_->cstats[config_.operatorId][slotIndex];
  if (slot.isWrite) {
    // Legacy write path gates all write I/O + checksum stats on warmup.
    if (afterWarmup) {
      ist.writeIos++;
      ist.writeBytes += res;
      ist.writeUsec += latUsec;
      ist.writeLatHist.addValue(latUsec);
      cst.writeCount += chksums;
      cst.writeNanos += csNs;
    }
  } else {
    // Legacy read path records I/O stats always, checksum stats after warmup.
    ist.readIos++;
    ist.readBytes += res;
    ist.readUsec += latUsec;
    ist.readLatHist.addValue(latUsec);
    if (afterWarmup) {
      cst.readCount += chksums;
      cst.readNanos += csNs;
    }
    // PairRole::SendDiskReads: hand the freshly-read bytes to the network layer
    // to send to our partner. This is what turns halcyon's paired mode from
    // synthetic (RAM-buffer bytes) into disk-derived (bytes that actually came
    // off flash). With a buffer pool we transfer ownership of the very buffer
    // the kernel read into -- no copy, matching HyperNode's read path -- and
    // acquire a fresh buffer when the slot is next armed; the SendItem's
    // deleter returns this buffer to the pool once the RPC drains. Without a
    // pool (should not happen in send modes) we fall back to a copy. The queue
    // is bounded, so when NIC is saturated blockingWrite pushes back and slows
    // the reactor -- the coupling we want. Only forwarded after warmup so
    // warmup-window bytes don't pollute the measurement window.
    if (config_.sendQueue != nullptr && afterWarmup) {
      SendItem item;
      item.size = static_cast<size_t>(res);
      if (config_.bufferPool != nullptr) {
        item.data = std::unique_ptr<uint8_t[], SendBufferDeleter>(
            static_cast<uint8_t*>(slot.buffer),
            SendBufferDeleter{config_.bufferPool});
        slot.buffer = nullptr; // ownership moved to the SendItem
      } else {
        // Fallback: no buffer pool, so we can't hand off ownership -- one copy
        // is unavoidable here. Not on the hot path (SendDisk* modes always
        // build a pool); kept for the SendNone / test paths.
        item.data = std::unique_ptr<uint8_t[], SendBufferDeleter>(
            new uint8_t[item.size], SendBufferDeleter{nullptr});
        // @lint-ignore CLANGSECURITY dst is a fresh new[item.size], count ==
        // item.size.
        folly::__folly_memcpy(item.data.get(), slot.buffer, item.size);
      }
      config_.sendQueue->blockingWrite(std::move(item));
    }
  }

  if (config_.backend == MetadataBackend::RocksDb && slot.isWrite) {
    // Data is durably on disk now (O_DIRECT|O_SYNC); persist the chunk-id ->
    // location mapping so the read path can find it. Persisted regardless of
    // warmup -- the mapping is correctness, not a measured statistic.
    ChunkLocation loc;
    loc.fileIndex = slot.fileIndex;
    loc.length = static_cast<uint32_t>(slot.ioSize);
    loc.offset = slot.offset;
    if (config_.flushQueue != nullptr) {
      // QFlush wired: hand the put to the shared flush pool so its put +
      // WAL-sync CPU is measured under getCpuNs(QFlush), off this reactor
      // thread. pendingFlush_ tracks the write so teardown can wait for it to
      // land before destroying mapper_; a worker decrements it once durable.
      // Count the write for throughput, but record no inline writeNanos -- the
      // cost moved to the pool.
      pendingFlush_.fetch_add(1, std::memory_order_relaxed);
      FlushRequest req;
      req.mapper = mapper_.get();
      req.id = slot.chunkId;
      req.loc = loc;
      req.pending = &pendingFlush_;
      // Bounded queue sized for headroom (see run()); blockingWrite applies
      // backpressure if flush workers ever fall behind the write rate.
      config_.flushQueue->blockingWrite(req);
      if (afterWarmup) {
        MetadataStats& mst = perfStats_->mstats[config_.operatorId][slotIndex];
        mst.writeCount++;
      }
    } else {
      // Inline put (no QFlush pool): time it so the metadata write cost is
      // attributable; gate the stat on warmup like the write I/O stats (the put
      // itself always runs).
      const uint64_t mdStart = current_nano();
      mapper_->put(slot.chunkId, loc);
      const uint64_t mdNs = current_nano() - mdStart;
      if (afterWarmup) {
        MetadataStats& mst = perfStats_->mstats[config_.operatorId][slotIndex];
        mst.writeCount++;
        mst.writeNanos += mdNs;
      }
    }
    // Write-populate the read cache (like HyperNode): a subsequent read of this
    // freshly-written chunk hits in-cache. No-op when the cache is disabled.
    if (cache_) {
      cache_->insert(slot.chunkId, loc);
    }
  }

  // Pooled mode: return the op's buffer. A no-op (null) when a read was handed
  // off to the send queue above; otherwise this releases writes' and
  // non-forwarded reads' buffers so the pool can hand them to the next op.
  if (config_.bufferPool != nullptr && slot.buffer != nullptr) {
    config_.bufferPool->release(static_cast<uint8_t*>(slot.buffer));
    slot.buffer = nullptr;
  }

  // SendDiskBoth zero-copy recv-write path: release the Thrift-received IOBuf
  // that backed the writev now that the kernel is done reading from it.
  slot.recvIOBuf.reset();

  slot.state = SlotState::Idle;
}

void IoCoreReactor::run(uint64_t warmupEndNs, uint64_t deadlineNs) {
  if (perfStats_ == nullptr) {
    throw std::runtime_error("IoCoreReactor::run requires a PerfStats");
  }
  if (config_.readRatio < 0.0 || config_.readRatio > 1.0) {
    throw std::runtime_error("IoCoreReactor::run requires readRatio in [0, 1]");
  }
  if (config_.readRatio > 0.0 && config_.readSizes.empty()) {
    throw std::runtime_error("IoCoreReactor::run requires non-empty readSizes");
  }
  if (config_.readRatio < 1.0 && config_.writeSizes.empty()) {
    throw std::runtime_error(
        "IoCoreReactor::run requires non-empty writeSizes");
  }
  warmupEndNs_ = warmupEndNs;

  // Spread the per-reactor iops across the queueDepth slots: each slot starts
  // an op at most once every targetSpacingNs. iops <= 0 leaves spacing at 0
  // (the reactor keeps the ring full).
  if (config_.iops > 0.0 && config_.queueDepth > 0) {
    targetSpacingNs_ = static_cast<uint64_t>(std::llround(
        1e9 * static_cast<double>(config_.queueDepth) / config_.iops));
  }

  openFiles();
  allocateSlots();

  if (config_.backend == MetadataBackend::RocksDb) {
    // Prefer caller-provided shared mapper (multi-reactor-per-mount);
    // otherwise open a private one. Both cases route through mapper_
    // (shared_ptr) so hot-path code doesn't branch.
    HalcyonChunkMapper::Options opts;
    opts.doDirectReads = config_.doDirectIo;
    opts.blockCacheBytes = static_cast<size_t>(FLAGS_rocks_block_cache_mb)
        << 20;
    if (config_.sharedMapper) {
      mapper_ = config_.sharedMapper;
    } else {
      mapper_ = std::make_shared<HalcyonChunkMapper>(
          config_.baseDir + "/chunkmap.rocksdb", opts);
    }
    // Shard the write-side chunk-id allocator across reactors sharing a
    // mapper. Each reactor's nextChunkId_ starts at
    // maxChunkId() + subIdx * kShardSize so concurrent puts allocate
    // disjoint id ranges. kShardSize picked big enough that a single
    // reactor won't exhaust it over any reasonable benchmark runtime.
    constexpr ChunkId kShardSize = 1ULL << 32; // 4B ids per reactor
    chunkIdShardBase_ =
        static_cast<ChunkId>(config_.reactorSubIdx) * kShardSize;
    // Start each reactor's write-side id allocator inside its shard so
    // concurrent puts across reactors sharing this mapper don't collide.
    nextChunkId_ = std::max<ChunkId>(mapper_->maxChunkId(), chunkIdShardBase_);
    nextOffset_.assign(fds_.size(), static_cast<uint64_t>(kChunkHeaderSize));
    // Read-through LRU in front of the chunk map (plan #1). Disabled (null)
    // when cacheCapacity == 0, leaving every read to hit RocksDB.
    if (config_.cacheCapacity > 0) {
      cache_ = std::make_unique<ChunkMetadataCache>(config_.cacheCapacity);
    }
  }

  io_uring ring;
  io_uring_params params{};
  const int ret =
      io_uring_queue_init_params(config_.ringEntries, &ring, &params);
  if (ret != 0) {
    // Destructor still frees slot buffers and closes fds.
    LOG(ERROR) << "io_uring_queue_init_params failed: " << ret;
    return;
  }

  // When the QMetadata pool is wired (RocksDb backend), reads are resolved
  // asynchronously: the reactor enqueues a chunk-map lookup and arms the disk
  // read only once a worker returns the location. Writes stay on the reactor.
  const bool asyncLookup = config_.requestQueue != nullptr;

  // Slots armed synchronously in the current iteration, tracked so they can be
  // unwound if the submit that would launch them fails -- otherwise they would
  // stay IoInflight forever and hang the drain loop at teardown.
  std::vector<int> armedThisIter;
  armedThisIter.reserve(slots_.size());

  // Hypernode-style reactor-loop CPU accounting (mirrors
  // hypernode/util/ThreadMgr.cpp:820-836). We track a rolling window of the
  // most recent idle-loop iterations to estimate polling-loop overhead, and
  // subtract that overhead from busy-loop iterations so useful CPU reflects
  // real I/O work rather than the busy-poll floor. Only counted after warmup.
  constexpr size_t kOverheadWindow = 4;
  uint64_t loopOverheadSum = 0;
  std::array<uint64_t, kOverheadWindow> lastIdleNs = {0, 0, 0, 0};
  size_t nextIdleIdx = 0;
  uint64_t reactorBusyNs = 0;
  uint64_t reactorIdleNs = 0;

  for (;;) {
    const uint64_t iterStartNs = current_nano();
    if (iterStartNs >= deadlineNs) {
      break;
    }
    const uint64_t now = iterStartNs;

    // Collect any resolved async lookups first: hits become pending disk reads,
    // misses free their slot.
    if (asyncLookup) {
      drainLookupResults(now);
    }

    int sqes = 0;
    // Arm disk reads for lookups that have already resolved to a hit.
    if (asyncLookup) {
      sqes += submitPendingReads(&ring);
    }

    // Arm a read or write (per readRatio) on every Idle slot whose pacing
    // deadline has passed. In async mode a read is handed to the QMetadata pool
    // as a lookup (no SQE); its disk read is armed later, once resolved.
    armedThisIter.clear();
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].state != SlotState::Idle || now < slots_[i].nextSubmitNs) {
        continue;
      }
      // Strict '<' so randDouble01()'s [0,1) range makes readRatio==0 strictly
      // write-only and readRatio==1 strictly read-only -- this matches the
      // readSizes/writeSizes validation below (no empty-distribution access).
      const bool doRead = folly::Random::randDouble01() < config_.readRatio;
      if (asyncLookup && doRead) {
        enqueueLookup(static_cast<int>(i), now);
        continue;
      }
      // SendDiskBoth mode: both queues set. Reactor consumes received
      // writes from recvQueue (peer's writes) and pushes its own writes
      // to sendQueue (for peer to persist). Local submitWrite is skipped
      // -- writes cross the network in this mode. In SendDiskReads mode
      // (sendQueue only), writes stay local as usual.
      const bool sendDiskBoth =
          (config_.recvQueue != nullptr && config_.sendQueue != nullptr);
      bool armed = false;
      if (doRead) {
        armed = submitRead(&ring, static_cast<int>(i));
      } else if (sendDiskBoth) {
        // Try to consume a partner's write. Only proceed if one is available;
        // otherwise skip this slot -- we'll retry next iteration.
        RecvItem item;
        if (config_.recvQueue->readIfNotEmpty(item)) {
          armed = submitRecvWrite(&ring, static_cast<int>(i), std::move(item));
        } else {
          // No partner write pending; also push our own generated write
          // to the peer via sendQueue so we don't stall. This keeps
          // pressure on the receiver's disk while balancing traffic.
          const int wsize = getPureIoSize(
              getIoSize(folly::Random::randDouble01(), &config_.writeSizes));
          if (wsize > 0 && now >= slots_[i].nextSubmitNs) {
            std::unique_ptr<uint8_t[], SendBufferDeleter> data;
            if (config_.bufferPool != nullptr) {
              // Pooled mode: an Idle slot holds no buffer, so take one from the
              // pool. Its content is arbitrary benchmark bytes; no fill needed.
              // Empty pool -> backpressure, retry next iteration.
              auto* buf = config_.bufferPool->acquire();
              if (buf == nullptr) {
                continue;
              }
              data = std::unique_ptr<uint8_t[], SendBufferDeleter>(
                  buf, SendBufferDeleter{config_.bufferPool});
            } else {
              // Fallback: no buffer pool. One copy is unavoidable; not on the
              // hot path (SendDisk* modes always build a pool).
              data = std::unique_ptr<uint8_t[], SendBufferDeleter>(
                  new uint8_t[static_cast<size_t>(wsize)],
                  SendBufferDeleter{nullptr});
              // Fill from reactor's own slot buffer (already dirty with
              // last-op data). Realistic enough for benchmark bytes.
              // @lint-ignore CLANGSECURITY dst is a fresh new[wsize], count ==
              // wsize.
              folly::__folly_memcpy(
                  data.get(), slots_[i].buffer, static_cast<size_t>(wsize));
            }
            SendItem sendItem{
                std::move(data),
                static_cast<size_t>(wsize),
                SendKind::WritePersist};
            // Non-blocking write: on a full queue sendItem is left intact and
            // its deleter returns the pooled buffer when it goes out of scope.
            (void)config_.sendQueue->write(std::move(sendItem));
            // Slot didn't do local disk work; keep it Idle for next pass.
          }
          continue;
        }
      } else {
        armed = submitWrite(&ring, static_cast<int>(i));
      }
      if (!armed) {
        break; // ring full; reap completions before preparing more
      }
      armedThisIter.push_back(static_cast<int>(i));
      ++sqes;
    }
    if (sqes > 0) {
      const int submitted = io_uring_submit(&ring);
      if (submitted < 0) {
        // The SQEs were not consumed by the kernel; revert the slots we armed
        // synchronously this iteration so they are retried and the drain loop
        // never waits on a completion that will never arrive. (Any pending-read
        // SQEs armed above are bounded by the timed drain below.)
        LOG(ERROR) << "io_uring_submit failed: " << submitted;
        for (const int idx : armedThisIter) {
          slots_[idx].state = SlotState::Idle;
        }
      }
    }

    // Reap whatever is ready; checksum runs inline in handleCompletion.
    bool reapedThisIter = false;
    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring, &cqe) == 0 && cqe != nullptr) {
      handleCompletion(cqe, warmupEndNs);
      io_uring_cqe_seen(&ring, cqe);
      reapedThisIter = true;
    }

    // Loop accounting: skip while warming up so overhead estimates and reported
    // ratios reflect only the measurement window.
    const uint64_t iterEndNs = current_nano();
    if (iterEndNs >= warmupEndNs) {
      const uint64_t iterNs = iterEndNs - iterStartNs;
      const bool idleLoop = (sqes == 0) && !reapedThisIter;
      if (idleLoop) {
        loopOverheadSum = loopOverheadSum -
            lastIdleNs[nextIdleIdx % kOverheadWindow] + iterNs;
        lastIdleNs[nextIdleIdx++ % kOverheadWindow] = iterNs;
        reactorIdleNs += iterNs;
      } else {
        const uint64_t overhead =
            std::min<uint64_t>(loopOverheadSum / kOverheadWindow, iterNs);
        reactorIdleNs += overhead;
        reactorBusyNs += iterNs - overhead;
      }
    }
  }

  // Publish per-reactor loop accounting onto slot 0 of this reactor's IoStats.
  // Reader aggregates by summing slot 0 across all reactors -- other slots keep
  // usefulBusyNs/usefulIdleNs at their default 0 so summation is safe.
  if (perfStats_ != nullptr && !perfStats_->istats.empty() &&
      !perfStats_->istats[config_.operatorId].empty()) {
    perfStats_->istats[config_.operatorId][0].usefulBusyNs = reactorBusyNs;
    perfStats_->istats[config_.operatorId][0].usefulIdleNs = reactorIdleNs;
  }

  // Drain outstanding work before teardown so the kernel is not still reading
  // from / writing into slot buffers when they are freed, and so neither a
  // lookup result nor a flush can touch the mapper after it is destroyed.
  // Outstanding work is any in-flight disk op, any unresolved async lookup, any
  // resolved-but-not-yet-armed read, or any chunk-map flush not yet made
  // durable by the QFlush pool. Use a bounded timed wait (defense in depth): a
  // kernel that never completes cannot hang teardown forever, and a hard wait
  // error breaks the loop instead of spinning on it.
  constexpr int64_t kDrainWaitSec = 5;
  constexpr int kMaxDrainTimeouts = 12; // give up after ~1 minute of silence
  int consecutiveTimeouts = 0;
  for (;;) {
    const uint64_t now = current_nano();
    if (asyncLookup) {
      drainLookupResults(now);
      if (submitPendingReads(&ring) > 0) {
        io_uring_submit(&ring);
      }
    }

    bool ioInflight = false;
    for (const OpSlot& slot : slots_) {
      if (slot.state == SlotState::IoInflight) {
        ioInflight = true;
        break;
      }
    }
    const bool lookupsPending =
        asyncLookup && (metadataInflight_ > 0 || !pendingReads_.empty());
    const bool flushesPending = config_.flushQueue != nullptr &&
        pendingFlush_.load(std::memory_order_acquire) > 0;
    if (!ioInflight && !lookupsPending && !flushesPending) {
      break;
    }

    if (ioInflight) {
      // A disk op is outstanding: do a bounded timed wait for its completion.
      io_uring_cqe* cqe = nullptr;
      __kernel_timespec ts{};
      ts.tv_sec = kDrainWaitSec;
      const int wret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
      if (wret == -ETIME) {
        if (++consecutiveTimeouts >= kMaxDrainTimeouts) {
          LOG(ERROR) << "Giving up io_uring drain after repeated timeouts; "
                     << "tearing down with ops still outstanding";
          break;
        }
        continue;
      }
      if (wret != 0) {
        LOG(ERROR) << "io_uring_wait_cqe_timeout failed during drain: " << wret;
        break;
      }
      consecutiveTimeouts = 0;
      if (cqe != nullptr) {
        handleCompletion(cqe, warmupEndNs);
        io_uring_cqe_seen(&ring, cqe);
      }
    } else {
      // Only async lookups remain (no disk op to block on); briefly yield so
      // the next iteration can collect worker results without a hot spin.
      // @lint-ignore CLANGTIDY
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }

  if (mapper_ != nullptr) {
    // All in-flight writes have drained (and put their mappings). Persist the
    // populated key range so a later read-only run knows it, then close the DB
    // before tearing the ring down -- no completion can touch it after this.
    mapper_->setMaxChunkId(
        std::max<ChunkId>(nextChunkId_, mapper_->maxChunkId()));
    mapper_.reset();
  }

  io_uring_queue_exit(&ring);

  if (config_.recvQueue != nullptr &&
      (zeroCopyRecvWrites_ != 0 || fallbackRecvWrites_ != 0)) {
    const uint64_t total = zeroCopyRecvWrites_ + fallbackRecvWrites_;
    const double zcPct =
        100.0 * static_cast<double>(zeroCopyRecvWrites_) / total;
    LOG(INFO) << "reactor[" << config_.baseDir << "#" << config_.reactorSubIdx
              << "] SendDiskBoth recvWrites=" << total
              << " zeroCopy=" << zeroCopyRecvWrites_ << " (" << zcPct
              << "%) fallback=" << fallbackRecvWrites_;
  }
}

IoUringEngine::IoUringEngine(HalcyonPools& pools, Config config)
    : pools_(pools), config_(std::move(config)) {}

void IoUringEngine::run() {
  const uint64_t now = current_nano();
  const uint64_t warmupEndNs =
      now + static_cast<uint64_t>(config_.warmupSec) * 1'000'000'000ull;
  const uint64_t deadlineNs = now +
      static_cast<uint64_t>(config_.warmupSec + config_.runtimeSec) *
          1'000'000'000ull;

  // Total reactor threads = mountPoints * reactorsPerMount. Each spawned
  // thread has its own io_uring ring, fds, OpSlots, and PerfStats row; they
  // share the on-disk fileset via independent O_DIRECT fds (kernel handles
  // concurrent reads/writes to the same files). operatorId is flattened
  // (mountIdx * reactorsPerMount + rIdx) so each reactor writes to its own
  // PerfStats row without collision.
  const size_t reactorsPerMount =
      std::max<size_t>(1, static_cast<size_t>(config_.reactorsPerMount));
  const size_t numReactors = config_.mountPoints.size() * reactorsPerMount;
  // Shared chunk mappers when reactorsPerMount > 1 with RocksDb backend.
  // RocksDB takes a per-directory lock, so two reactors on the same mount
  // can't each open the same chunkmap.rocksdb path -- build one mapper per
  // mount here, share it via shared_ptr across reactors on that mount.
  // Each reactor still has its own nextChunkId_ starting offset (see
  // reactorSubIdx) so their concurrent writes allocate disjoint id ranges.
  // rocksdb::DB Get/Put/Batch are already thread-safe; HalcyonChunkMapper's
  // only mutable state (maxChunkId_) is set once at fileset-create and
  // read-only during a benchmark run.
  // Always build per-mount mappers up front so every reactor shares one LRU
  // block cache across all mounts (mirrors HyperNode's sharedBlockCacheSizeMiB
  // -- a single cache split across every mount's CF, not per-mount). Sharing
  // gives a hot chunk on any drive one shared entry instead of N copies, and
  // amortizes the total cache budget for better hit rate.
  // --rocks_block_cache_mb is per-mount; total = flag × mount_count (10 GiB on
  // T8 default: 640 × 16).
  std::vector<std::shared_ptr<HalcyonChunkMapper>> perMountMappers;
  if (config_.backend == MetadataBackend::RocksDb) {
    perMountMappers.reserve(config_.mountPoints.size());
    const size_t totalBlockCacheBytes =
        static_cast<size_t>(FLAGS_rocks_block_cache_mb) *
        config_.mountPoints.size() * (1ul << 20);
    auto sharedCache = rocksdb::NewLRUCache(totalBlockCacheBytes);
    HalcyonChunkMapper::Options opts;
    opts.doDirectReads = config_.doDirectIo;
    opts.sharedBlockCache = sharedCache;
    for (const auto& mp : config_.mountPoints) {
      perMountMappers.push_back(
          std::make_shared<HalcyonChunkMapper>(mp + "/chunkmap.rocksdb", opts));
    }
  }

  // Async chunk-map lookup wiring (RocksDb backend with QMetadata workers > 0).
  // The request queue is shared by all reactors; each reactor has its own
  // result queue. Capacities cover the worst case -- every slot holding one
  // outstanding lookup -- so neither the reactors' enqueues nor the workers'
  // result posts ever block. These outlive the reactors and the workers (the
  // workers hold raw mapper pointers carried in the requests, and each reactor
  // drains all of its lookups before destroying its mapper).
  const bool asyncLookup =
      config_.backend == MetadataBackend::RocksDb && config_.qmetaThreads > 0;
  std::unique_ptr<LookupRequestQueue> requestQueue;
  std::vector<std::unique_ptr<LookupResultQueue>> resultQueues;
  std::vector<LookupResultQueue*> resultQueuePtrs;
  std::unique_ptr<MetadataWorker> worker;
  std::vector<folly::Future<folly::Unit>> workerFutures;

  if (asyncLookup) {
    const size_t perReactorSlots = std::max<size_t>(1, config_.threadsPerDisk);
    requestQueue =
        std::make_unique<LookupRequestQueue>(numReactors * perReactorSlots);
    // DMPSCQueue capacity is approximate -- folly reserves ~10% slack plus
    // per-producer credit and explicitly is not a semaphore -- so size the
    // result queue well above the true max outstanding (at most one result per
    // slot) to keep it far from full, so the worker's try_enqueue always
    // succeeds. Still bounded.
    const size_t resultQueueCapacity =
        std::max<size_t>(perReactorSlots * 8, 1024);
    resultQueues.reserve(numReactors);
    resultQueuePtrs.reserve(numReactors);
    for (size_t i = 0; i < numReactors; ++i) {
      resultQueues.push_back(
          std::make_unique<LookupResultQueue>(resultQueueCapacity));
      resultQueuePtrs.push_back(resultQueues.back().get());
    }
    worker = std::make_unique<MetadataWorker>(
        requestQueue.get(),
        resultQueuePtrs,
        kLookupBatch,
        config_.busyPollDataPath);
    auto& qpool = pools_.pool(PoolRole::QMetadata);
    workerFutures.reserve(config_.qmetaThreads);
    for (int i = 0; i < config_.qmetaThreads; ++i) {
      workerFutures.push_back(
          folly::via(&qpool, [w = worker.get()] { w->run(); }));
    }
  }

  // Async chunk-map write wiring (RocksDb backend with QFlush workers > 0). One
  // shared MPMC request queue feeds all flush workers; each reactor enqueues
  // its completed writes' chunk-map puts here. The queue and worker outlive the
  // reactors -- the workers reach each mapper through the queued requests, and
  // every reactor drains its outstanding flushes before destroying its mapper.
  const bool asyncFlush =
      config_.backend == MetadataBackend::RocksDb && config_.qflushThreads > 0;
  std::unique_ptr<FlushRequestQueue> flushQueue;
  std::unique_ptr<FlushWorker> flushWorker;
  std::vector<folly::Future<folly::Unit>> flushWorkerFutures;

  if (asyncFlush) {
    const size_t perReactorSlots = std::max<size_t>(1, config_.threadsPerDisk);
    // Sized for headroom over the true max outstanding (at most one flush per
    // slot across all reactors). handleCompletion enqueues with blockingWrite,
    // so this only needs to stay ahead of the workers to avoid stalling a
    // reactor's busy-poll loop on backpressure. Still bounded.
    const size_t flushQueueCapacity =
        std::max<size_t>(numReactors * perReactorSlots * 8, 1024);
    flushQueue = std::make_unique<FlushRequestQueue>(flushQueueCapacity);
    flushWorker = std::make_unique<FlushWorker>(
        flushQueue.get(), kFlushBatch, config_.busyPollDataPath);
    auto& qpool = pools_.pool(PoolRole::QFlush);
    flushWorkerFutures.reserve(config_.qflushThreads);
    for (int i = 0; i < config_.qflushThreads; ++i) {
      flushWorkerFutures.push_back(
          folly::via(&qpool, [w = flushWorker.get()] { w->run(); }));
    }
  }

  std::vector<std::thread> reactors;
  reactors.reserve(numReactors);
  for (size_t m = 0; m < config_.mountPoints.size(); ++m) {
    for (size_t r = 0; r < reactorsPerMount; ++r) {
      const size_t i = m * reactorsPerMount + r;
      ReactorConfig rc;
      rc.baseDir = config_.mountPoints[m];
      rc.operatorId = static_cast<int>(i);
      rc.queueDepth = config_.threadsPerDisk;
      rc.ringEntries = config_.ringEntries;
      rc.doDirectIo = config_.doDirectIo;
      rc.readRatio = config_.readRatio;
      rc.iops = config_.iops;
      rc.readSizes = config_.readSizes;
      rc.writeSizes = config_.writeSizes;
      rc.backend = config_.backend;
      rc.cacheCapacity = config_.cacheCapacity;
      rc.readLocality = config_.readLocality;
      rc.localityWindow = config_.localityWindow;
      if (asyncLookup) {
        rc.requestQueue = requestQueue.get();
        rc.resultQueue = resultQueuePtrs.at(i);
      }
      if (asyncFlush) {
        rc.flushQueue = flushQueue.get();
      }
      // Shared across all reactors; caller owns the queue and outlives the run.
      rc.sendQueue = config_.sendQueue;
      // Shared aligned-buffer pool for the disk-derived paired modes; caller
      // owns it and it outlives both the reactors and the network workers.
      rc.bufferPool = config_.bufferPool;
      // SendDiskBoth mode: same fanout as sendQueue. All reactors on all
      // mounts consume received writes from a single shared MPMC queue --
      // the Thrift server round-robins incoming writes across reactors.
      rc.recvQueue = config_.recvQueue;
      // queueDepth decoupled from threadsPerDisk. 0 keeps legacy behavior
      // (opSlots == thread count); > 0 lets one reactor thread juggle N
      // in-flight ops via deep io_uring ring, matching Hypernode's shape.
      if (config_.queueDepth > 0) {
        rc.queueDepth = config_.queueDepth;
      }
      // Multi-reactor plumbing. When reactorsPerMount > 1 + RocksDb, all
      // reactors on the same mount share one HalcyonChunkMapper; each
      // reactor's own nextChunkId_ is offset by reactorSubIdx * kShard to
      // avoid write-side id collisions across reactors sharing a mapper.
      rc.reactorSubIdx = static_cast<int>(r);
      rc.reactorsPerMount = static_cast<int>(reactorsPerMount);
      if (!perMountMappers.empty()) {
        rc.sharedMapper = perMountMappers[m];
      }

      PerfStats* perfStats = config_.perfStats;
      reactors.push_back(
          pools_.makeReactorThread([rc = std::move(rc),
                                    perfStats,
                                    mountPoint = config_.mountPoints[m],
                                    reactorIdx = r,
                                    warmupEndNs = warmupEndNs,
                                    deadlineNs = deadlineNs]() mutable {
            try {
              IoCoreReactor reactor(std::move(rc), perfStats);
              reactor.run(warmupEndNs, deadlineNs);
            } catch (const std::exception& e) {
              LOG(ERROR) << "IO-Core reactor for " << mountPoint << " #"
                         << reactorIdx << " failed: " << e.what();
            }
          }));
    }
  }
  for (auto& reactor : reactors) {
    reactor.join();
  }

  // Every reactor has drained its lookups and destroyed its mapper, so no
  // queued request references a live mapper. Stop the workers and wait for
  // their loops (and final drain) to finish before the queues are destroyed.
  if (worker != nullptr) {
    worker->stop();
    folly::collectAll(std::move(workerFutures)).get();
  }

  // Likewise, every reactor has drained its flushes and destroyed its mapper,
  // so no queued request references a live mapper. Stop the flush workers and
  // wait for their loops (and final drain) to finish before the queue is
  // destroyed.
  if (flushWorker != nullptr) {
    flushWorker->stop();
    folly::collectAll(std::move(flushWorkerFutures)).get();
  }
}

} // namespace facebook::halcyon
