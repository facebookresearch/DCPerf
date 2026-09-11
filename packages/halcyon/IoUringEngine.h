// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <folly/MPMCQueue.h>
#include <folly/Random.h>

#include "Common.h" // IoSizeRange, MetadataBackend
#include "FlushWorker.h" // FlushRequestQueue
#include "HalcyonChunkMapper.h" // ChunkId, HalcyonChunkMapper
#include "MetadataWorker.h" // LookupRequestQueue, LookupResultQueue
#include "ThreadPools.h"

// Forward declarations of the liburing C structs so this header does not pull
// in <liburing.h> (only pointers are referenced below).
struct io_uring;
struct io_uring_cqe;

// Forward declare folly::IOBuf so this header does not pull in
// <folly/io/IOBuf.h> (only unique_ptr<IOBuf> is referenced below).
namespace folly {
class IOBuf;
} // namespace folly

namespace facebook::halcyon {

class PerfStats;
class ChunkMetadataCache;

/// SendItem::kind distinguishes reads (peer discards) from writes (peer
/// persists to disk) in SendDiskBoth mode. In SendDiskReads mode all items
/// are ReadDiscard.
enum class SendKind : uint8_t {
  ReadDiscard = 0, // partner just receives and drops (SendDiskReads path)
  WritePersist = 1, // partner routes to reactor's recvQueue -> io_uring write
};

/// Fixed pool of aligned, O_DIRECT-capable I/O buffers shared by every reactor
/// in the disk-derived paired modes (PairRole::SendDiskReads / SendDiskBoth).
/// It replaces the per-slot fixed buffers so a completed read's buffer can be
/// handed to the network layer by *moving ownership* instead of memcpy: a
/// reactor acquires a buffer to arm each op and, on completion, either returns
/// it (writes, discarded reads) or transfers it into a SendItem whose deleter
/// returns it once the RPC drains. This removes the per-read copy that has no
/// analog in the HyperNode read path halcyon models (and that burns memory
/// bandwidth). Backpressure is preserved: sized to totalSlots + sendQueue
/// capacity, the pool never runs dry before the sendQueue's bounded
/// blockingWrite becomes the limiter.
///
/// acquire() is called only from reactor threads; release() may run on a
/// NetworkWorker thread (via the SendItem/IOBuf deleter), so the free list is a
/// thread-safe MPMCQueue. Every buffer is posix_memalign'd (kMinIoSize aligned,
/// `bufferSize` bytes) and freed at pool destruction. The pool must outlive
/// both the reactors and the network workers (owned by HalcyonServiceHandler,
/// like sendQueue_).
class IoBufferPool {
 public:
  IoBufferPool(size_t count, size_t bufferSize);
  ~IoBufferPool();

  IoBufferPool(const IoBufferPool&) = delete;
  IoBufferPool& operator=(const IoBufferPool&) = delete;
  IoBufferPool(IoBufferPool&&) = delete;
  IoBufferPool& operator=(IoBufferPool&&) = delete;

  // Non-blocking: returns a buffer, or nullptr when the pool is exhausted (the
  // caller leaves the slot Idle and retries next iteration -- backpressure).
  uint8_t* acquire();
  // Returns a buffer to the pool. Never blocks (capacity == total buffers).
  // A null buffer is ignored (a read handed off via SendItem leaves the slot
  // holding nullptr).
  void release(uint8_t* buffer);

 private:
  folly::MPMCQueue<uint8_t*> free_;
  std::vector<uint8_t*> all_; // owns every buffer, for teardown
};

/// Deleter for SendItem::data. When `pool` is set the buffer is returned to
/// that shared IoBufferPool; otherwise it was heap-allocated (new uint8_t[])
/// and is delete[]'d. This lets the network worker adopt the buffer into an
/// IOBuf and dispose of it correctly once the RPC drains, with no copy.
struct SendBufferDeleter {
  IoBufferPool* pool{nullptr};
  void operator()(uint8_t* p) const {
    if (p == nullptr) {
      return;
    }
    if (pool != nullptr) {
      pool->release(p);
    } else {
      delete[] p;
    }
  }
};

/// One data buffer forwarded from a reactor to the network layer for
/// transmission to a partner (PairRole::SendDiskReads / SendDiskBoth).
/// Owns the byte buffer via unique_ptr with a pool-aware deleter: in the
/// pooled path the buffer is the very one the kernel read into (moved out of
/// the reactor slot, no copy) and is returned to the IoBufferPool once the RPC
/// drains; the legacy fallback owns a heap buffer freed with delete[].
/// `kind` tags the send as read-derived (default, peer discards) or
/// write-payload (SendDiskBoth, peer submits as io_uring write).
struct SendItem {
  std::unique_ptr<uint8_t[], SendBufferDeleter> data;
  size_t size{0};
  SendKind kind{SendKind::ReadDiscard};
};

/// Bounded MPMC queue that couples the reactor (producer) to NetworkOperator
/// (consumer). When full, the reactor's push blocks, which back-pressures
/// disk-read submission and naturally caps file_xput at whatever the NIC can
/// drain. Sized generously (a few hundred slots) so momentary NIC hiccups
/// don't stall the reactor but sustained NIC saturation does.
using SendDataQueue = folly::MPMCQueue<SendItem>;

/// One incoming write payload received from the paired partner over the
/// network (PairRole::SendDiskBoth only). Holds the raw Thrift-received
/// IOBuf chain by value -- co_sendData std::moves `*request->payload()`
/// into it, so no intermediate heap buffer is allocated on the receive
/// path. The reactor then cursor-pulls directly from the chain into its
/// aligned O_DIRECT pool buffer for the io_uring pwrite (one alignment
/// copy, unavoidable while Thrift IOBufs aren't sector-aligned; matches
/// what HN does for the same reason). Size is derived from
/// `data->computeChainDataLength()` which is O(1) with cached lengths.
struct RecvItem {
  std::unique_ptr<folly::IOBuf> data;
};

/// Bounded MPMC queue coupling the Thrift server (producer, via
/// HalcyonServiceHandler::co_sendData) to reactors (consumers) in
/// SendDiskBoth mode. When full, co_sendData's blockingWrite awaits,
/// which back-pressures the sender's Thrift RPC layer -- the network
/// side of the write pipeline slows down when receiver disk can't keep
/// up. Sized to absorb short bursts without stalling.
using RecvDataQueue = folly::MPMCQueue<RecvItem>;

/// File layout selector: a fixed dirs x files grid per mount point. Picks a
/// random file and builds its partial path (joined with the mount point).
/// Reused by the reactor's openFiles().
struct FileSelector {
  int dirs = 0;
  int files = 0;
  int randomFile() {
    return folly::Random::rand32(dirs * files);
  }
  std::string partialPath(int fn) {
    int d = fn / files;
    int f = fn - d * files;
    return fmt::format("/d{}/f{}", d, f);
  }
};

/// State of a single in-flight-op slot. Checksum runs inline on the reactor
/// (Option B), so there is no separate CHECKSUM_INFLIGHT state: a slot goes
/// Idle -> IoInflight -> (inline checksum) -> Idle.
///
/// With the async QMetadata pool wired in (RocksDb backend only), a read slot
/// first parks in MetadataInflight while a worker resolves its chunk-map
/// lookup, then transitions to IoInflight once the location comes back: Idle ->
/// (enqueue lookup) MetadataInflight -> (hit) IoInflight -> Idle, or
/// MetadataInflight -> (miss) Idle.
enum class SlotState {
  Idle, // available to launch the next op
  MetadataInflight, // a chunk-map lookup is outstanding on the QMetadata pool
  IoInflight, // an io_uring read/write is outstanding for this slot
};

/// One in-flight I/O slot owned by a reactor. Each slot has its own aligned,
/// O_DIRECT-capable buffer so reads/writes never share memory across slots.
struct OpSlot {
  void* buffer{nullptr}; // kMinIoSize-aligned, kMaxFileSize bytes
  int fd{-1}; // file targeted by the current op
  uint64_t offset{0}; // byte offset within the file
  int ioSize{0}; // bytes for the current op
  uint64_t ioStartNs{0}; // disk-submit timestamp, for I/O latency accounting
  // Timestamp the op left Idle: disk-submit time for writes/manifest reads, or
  // lookup-enqueue time for async RocksDb reads. Pacing is charged from here so
  // a read's metadata wait counts against its iops spacing.
  uint64_t opStartNs{0};
  uint64_t nextSubmitNs{0}; // pacing deadline; do not re-arm until now >= this
  bool isWrite{false}; // true if the in-flight op is a write
  SlotState state{SlotState::Idle};
  // RocksDb backend only: identity of the in-flight op so handleCompletion can
  // persist the chunk-id -> location mapping after a write completes.
  uint32_t fileIndex{0}; // index into fds_ of the current op's file
  ChunkId chunkId{0}; // chunk id of the current op
  // SendDiskBoth zero-copy write path only: when submitRecvWrite skipped the
  // pool memcpy and submitted io_uring_prep_writev with an iovec pointing at
  // the Thrift-received IOBuf's own buffer, this holds the IOBuf so its
  // memory stays live until the kernel finishes the write. Reset in
  // handleCompletion. Null in the fallback (memcpy'd) path.
  std::unique_ptr<folly::IOBuf> recvIOBuf;
};

/// Static configuration for a single reactor (one mount point). The run-time
/// window (warmup/deadline) is passed separately to run().
struct ReactorConfig {
  std::string baseDir; // mount point whose fileset this reactor drives
  int operatorId{0}; // index into PerfStats rows (one per mount point)
  int queueDepth{1}; // number of OpSlots == max in-flight ops
  unsigned ringEntries{256}; // io_uring submission/completion ring size
  bool doDirectIo{true}; // open files O_DIRECT (false on tmpfs/tests)
  double readRatio{1.0}; // fraction of ops that are reads (vs writes)
  double iops{0.0}; // target IOPS for this reactor; <= 0 == unthrottled
  // Read-key access locality (RocksDb backend). With probability readLocality a
  // read targets the most-recently-allocated `localityWindow` chunk ids instead
  // of a uniform pick, so an LRU cache's hit rate emerges from reuse. 0 (or a
  // zero window) keeps the uniform key selection.
  double readLocality{0.0};
  ChunkId localityWindow{0};
  // ChunkMetadataCache capacity in entries (RocksDb backend). 0 disables the
  // per-reactor read cache; > 0 builds an LRU of that many chunk-id -> location
  // entries, so hot reads (see readLocality) skip the DB lookup.
  size_t cacheCapacity{0};
  std::vector<IoSizeRange> readSizes; // I/O size distribution for reads
  std::vector<IoSizeRange> writeSizes; // I/O size distribution for writes
  MetadataBackend backend{MetadataBackend::Manifest}; // I/O addressing backend
  // Async chunk-map lookup wiring (RocksDb backend only). When requestQueue is
  // non-null the reactor offloads reads to the shared QMetadata pool instead of
  // looking up inline; both are owned by IoUringEngine::run and outlive the
  // reactor. Null on both leaves the inline lookup path (M2/M5) in place.
  LookupRequestQueue* requestQueue{nullptr}; // reactors -> workers (shared)
  LookupResultQueue* resultQueue{nullptr}; // workers -> this reactor
  // Async chunk-map write wiring (RocksDb backend only). When non-null the
  // reactor offloads each completed write's chunk-map put to the shared QFlush
  // pool instead of putting inline; owned by IoUringEngine::run and outlives
  // the reactor (which drains all of its flushes before destroying its mapper).
  // Null leaves the inline put path (M2/M5) in place.
  FlushRequestQueue* flushQueue{nullptr}; // this reactor -> flush workers
  // Disk-derived paired mode (PairRole::SendDiskReads). When non-null, each
  // completed read's buffer is handed to this queue for the network layer to
  // send to the partner. Null keeps the legacy behavior (read completes,
  // buffer discarded, no network coupling).
  SendDataQueue* sendQueue{nullptr};
  // Shared buffer pool for the disk-derived paired modes. When non-null the
  // reactor draws every op's I/O buffer from this pool instead of using a
  // fixed per-slot buffer, so a completed read's buffer can be moved into a
  // SendItem (zero-copy) rather than memcpy'd; writes and discarded reads
  // return their buffer on completion. Null keeps the legacy per-slot buffers
  // allocated by allocateSlots(). Set together with sendQueue.
  IoBufferPool* bufferPool{nullptr};
  // Fully-coupled paired mode (PairRole::SendDiskBoth). When non-null the
  // reactor polls this queue for write payloads received from the partner
  // via the Thrift server -- each RecvItem consumes an OpSlot and submits
  // an io_uring pwrite with the received buffer instead of generating
  // write data locally from RAM. Shared across all reactors on the same
  // mount so the server can round-robin incoming writes without pinning.
  // Null keeps the legacy behavior (writes locally generated from a
  // pre-filled RAM buffer, receiver discards).
  RecvDataQueue* recvQueue{nullptr};
  // Shared chunk mapper for multi-reactor-per-mount + RocksDb. When set,
  // the reactor uses this mapper instead of opening its own. Populated in
  // IoUringEngine::run() with one mapper per mount, shared across all
  // reactorsPerMount reactors on that mount. Null in single-reactor mode.
  std::shared_ptr<HalcyonChunkMapper> sharedMapper;
  // Reactor's local index within its mount (0..reactorsPerMount-1). Used
  // to shard chunk-id allocation across reactors sharing a mapper so
  // concurrent writes don't collide on the same id.
  int reactorSubIdx{0};
  int reactorsPerMount{1};
};

/// One IO-Core reactor: owns a single io_uring ring, the pre-opened fd cache
/// for one mount point's fileset, and a fixed set of OpSlots (= ring queue
/// depth). Modeled on HyperNode's IOUringPsa: a pinned thread busy-polls
/// completions rather than blocking one thread per in-flight I/O.
///
/// B6 (this diff) adds pacing: each slot carries a per-slot submit deadline so
/// the reactor only re-arms a slot once enough time has passed to honor the
/// configured `iops` (spread evenly across the queueDepth slots). With iops <=
/// 0 the reactor is unthrottled and keeps the ring full.
class IoCoreReactor {
 public:
  IoCoreReactor(ReactorConfig config, PerfStats* perfStats);
  ~IoCoreReactor();

  // Owns fds + raw buffers + the io_uring ring; neither copyable nor movable.
  IoCoreReactor(const IoCoreReactor&) = delete;
  IoCoreReactor& operator=(const IoCoreReactor&) = delete;
  IoCoreReactor(IoCoreReactor&&) = delete;
  IoCoreReactor& operator=(IoCoreReactor&&) = delete;

  /// Reads `{baseDir}/manifest` and opens every file in the set, caching fds by
  /// flattened file index. Throws std::runtime_error on manifest/open failure.
  void openFiles();

  /// Allocates `queueDepth` OpSlots, each with a kMinIoSize-aligned,
  /// kMaxFileSize buffer suitable for O_DIRECT. Throws std::bad_alloc on
  /// allocation failure.
  void allocateSlots();

  /// Full reactor lifecycle: openFiles() + allocateSlots() + ring init, then
  /// arm reads/writes on due Idle slots (respecting pacing) + reap completions
  /// (inline checksum) until `deadlineNs`, drain outstanding ops, and tear the
  /// ring down. Checksum (and, for writes, all) stats are only recorded once
  /// now >= `warmupEndNs`. Requires a non-null PerfStats and the I/O size
  /// distributions needed by readRatio (throws otherwise).
  void run(uint64_t warmupEndNs, uint64_t deadlineNs);

  const std::vector<int>& fds() const {
    return fds_;
  }
  const std::vector<OpSlot>& slots() const {
    return slots_;
  }

 private:
  void readManifest();
  // Prepares + arms a read SQE for the given Idle slot. Returns false (without
  // touching the slot) when the ring has no free SQE.
  bool submitRead(io_uring* ring, int slotIndex);
  // Prepares + arms a write SQE for the given Idle slot. Returns false (without
  // touching the slot) when the ring has no free SQE.
  bool submitWrite(io_uring* ring, int slotIndex);
  // SendDiskBoth mode: submit a write whose payload came from the partner
  // over the network (RecvItem popped from config_.recvQueue). Same on-disk
  // layout as submitWrite but the source bytes are the received buffer.
  bool submitRecvWrite(io_uring* ring, int slotIndex, RecvItem&& item);
  // Async RocksDb read path (only when config_.requestQueue is set). Picks a
  // chunk id, enqueues a lookup on the shared QMetadata request queue, and
  // parks the slot in MetadataInflight (no SQE consumed). `now` is the enqueue
  // time, recorded as the slot's opStartNs for pacing.
  void enqueueLookup(int slotIndex, uint64_t now);
  // Drains all resolved lookups from this reactor's result queue: records the
  // metadata-read stat (gated on now >= warmupEndNs_), queues hits as pending
  // disk reads, and returns misses to Idle.
  void drainLookupResults(uint64_t now);
  // Prepares + arms disk-read SQEs for as many pending (resolved-hit) lookups
  // as the ring has free SQEs. Returns the number of SQEs prepared.
  int submitPendingReads(io_uring* ring);
  // Prepares + arms one disk-read SQE for a resolved hit at `loc`. Returns
  // false (without touching the slot) when the ring has no free SQE.
  bool
  submitMappedRead(io_uring* ring, int slotIndex, const ChunkLocation& loc);
  // Records I/O stats + inline checksum for a completed op, sets the slot's
  // next pacing deadline, and marks it Idle. Checksum (and write) stats are
  // gated on now >= warmupEndNs.
  void handleCompletion(io_uring_cqe* cqe, uint64_t warmupEndNs);

  ReactorConfig config_;
  PerfStats* perfStats_;
  // Target spacing between consecutive op *starts* on a single slot, in ns.
  // 0 == unthrottled. Derived from iops / queueDepth in run().
  uint64_t targetSpacingNs_{0};
  // End of the warmup window (ns), captured in run() so submitRead can gate the
  // metadata-lookup timing the same way handleCompletion gates its stats.
  uint64_t warmupEndNs_{0};
  int dirs_{0};
  int files_{0};
  std::vector<int> fds_;
  std::vector<OpSlot> slots_;
  // RocksDb backend only (null for Manifest): per-mount-point chunk mapper plus
  // the write-path allocators -- a bump offset cursor per fd and a monotonic
  // chunk-id counter.
  // Owning or shared depending on ReactorConfig::sharedMapper. Made
  // shared_ptr so multi-reactor-per-mount (RocksDb) can share one mapper.
  std::shared_ptr<HalcyonChunkMapper> mapper_;
  // Distinct starting offset for per-reactor chunk-id allocation when
  // sharing a mapper. Used in place of the plain maxChunkId() base to
  // give each reactor a disjoint id range.
  ChunkId chunkIdShardBase_{0};
  // Per-reactor read-through LRU in front of mapper_ (plan #1). Built in run()
  // alongside mapper_ when config_.cacheCapacity > 0; null (and bypassed) when
  // the cache is disabled.
  std::unique_ptr<ChunkMetadataCache> cache_;
  std::vector<uint64_t> nextOffset_;
  uint64_t nextChunkId_{0};
  // Async-lookup bookkeeping (used only when config_.requestQueue is set): a
  // resolved hit awaiting a disk-read SQE, and the count of slots currently
  // parked in MetadataInflight (so teardown can wait them out).
  struct MappedRead {
    int slotIndex{0};
    ChunkLocation loc;
  };
  std::vector<MappedRead> pendingReads_;
  int metadataInflight_{0};
  // SendDiskBoth recv-write path opportunistic-aliasing counters. Bumped in
  // submitRecvWrite: zeroCopy when the incoming IOBuf was unchained + word
  // aligned + LBA-multiple length and we submitted io_uring_prep_writev
  // directly (no alignment memcpy), fallback when we cursor-pulled into the
  // aligned pool buffer. Logged at reactor teardown to confirm the fast path
  // is dominant and quantify memBW savings vs the pure-fallback baseline.
  // Matches the pattern of HN's IoCore::transferIOBufData (see
  // hypernode/data/dpc/IoCore.cpp:591-610), which uses the same weak
  // alignment predicate (word-aligned, LBA-length).
  uint64_t zeroCopyRecvWrites_{0};
  uint64_t fallbackRecvWrites_{0};
  // Outstanding chunk-map flushes handed to the QFlush pool (used only when
  // config_.flushQueue is set). A flush worker decrements this once a mapping
  // is durable; teardown waits it to zero before destroying mapper_, since the
  // workers reach mapper_ through the queued requests.
  std::atomic<int> pendingFlush_{0};
};

/// Raw-liburing file-I/O engine modeled on HyperNode's IO Core
/// (fbcode/hypernode/data/psa/iouring/IOUringPsa.cpp): one pinned reactor
/// thread per mount point, each owning a single io_uring ring and busy-polling
/// completions, instead of one blocked thread per in-flight I/O.
///
/// Standalone and not yet wired into the benchmark; later diffs swap the two
/// callers.
class IoUringEngine {
 public:
  struct Config {
    /// One reactor thread (and one ring) is created per mount point.
    std::vector<std::string> mountPoints;
    int warmupSec{0};
    int runtimeSec{0};
    /// io_uring submission/completion ring size (entries).
    unsigned ringEntries{256};
    /// Ring queue depth == number of OpSlots per reactor.
    int threadsPerDisk{1};
    /// Open files with O_DIRECT (must be false on filesystems like tmpfs that
    /// reject O_DIRECT, e.g. in unit tests).
    bool doDirectIo{true};
    /// Fraction of ops that are reads (vs writes).
    double readRatio{1.0};
    /// Target IOPS per mount point (per reactor); <= 0 == unthrottled.
    double iops{0.0};
    /// I/O size distribution for reads.
    std::vector<IoSizeRange> readSizes;
    /// I/O size distribution for writes.
    std::vector<IoSizeRange> writeSizes;
    /// Metadata addressing backend: the legacy manifest grid, or the RocksDB
    /// chunk mapper (one DB per mount point at {baseDir}/chunkmap.rocksdb).
    MetadataBackend backend{MetadataBackend::Manifest};
    /// Number of QMetadata lookup workers to run on pools.pool(QMetadata).
    /// 0 (default) keeps chunk-map lookups inline on the reactor thread; > 0
    /// (RocksDb backend only) offloads reads to the shared async worker pool.
    /// Should match the PoolConfig.qmetaThreads used to build `pools`.
    int qmetaThreads{0};
    /// Number of QFlush write-offload workers to run on pools.pool(QFlush).
    /// 0 (default) keeps chunk-map puts inline on the reactor thread; > 0
    /// (RocksDb backend only) offloads writes to the shared QFlush pool, so the
    /// put + WAL-sync CPU is measured under getCpuNs(QFlush) instead of
    /// QIOThread. Should match the PoolConfig.qflushThreads used to build
    /// `pools`.
    int qflushThreads{0};
    /// ChunkMetadataCache capacity in entries per reactor (RocksDb backend).
    /// 0 (default) disables the per-reactor read cache; > 0 builds an LRU of
    /// that many chunk-id -> location entries so hot reads skip the DB lookup.
    size_t cacheCapacity{0};
    /// Read-key access locality (RocksDb backend). With probability
    /// readLocality a read targets the most-recently-allocated `localityWindow`
    /// chunk ids instead of a uniform pick, so the cache's hit rate emerges
    /// from reuse. 0 (default) keeps the uniform key selection.
    double readLocality{0.0};
    ChunkId localityWindow{0};
    /// Disk-derived paired mode (PairRole::SendDiskReads). Non-null forwards
    /// every completed read's bytes onto this queue for the network layer to
    /// send to the partner. The queue is shared across all reactors and owned
    /// by the caller (HalcyonServiceHandler). Null keeps legacy behavior.
    SendDataQueue* sendQueue{nullptr};
    /// Shared aligned-buffer pool for the disk-derived paired modes. Non-null
    /// makes reactors draw op buffers from the pool so completed reads hand
    /// their buffer to the network layer with no copy. Shared across all
    /// reactors and owned by the caller (HalcyonServiceHandler), which sizes it
    /// to totalSlots + sendQueue capacity. Set together with sendQueue; null
    /// keeps the legacy per-slot buffers.
    IoBufferPool* bufferPool{nullptr};
    /// Fully-coupled paired mode (PairRole::SendDiskBoth). Non-null makes
    /// every reactor consume write payloads from this shared queue instead
    /// of generating writes locally from RAM. Owned by HalcyonServiceHandler,
    /// populated by co_sendData when the Thrift server receives a Write op
    /// from the partner. Null keeps legacy write-from-RAM behavior.
    RecvDataQueue* recvQueue{nullptr};
    /// Convert MetadataWorker + FlushWorker idle-loop sleeps to
    /// `folly::asm_volatile_pause` (busy-poll). Matches Hypernode's
    /// ThreadMgr::threadWorker where all data-path pools spin at 100% per
    /// thread. See RuntimeConfiguration.busyPollDataPath in halcyon.thrift.
    bool busyPollDataPath{false};
    /// Number of OpSlots per reactor (max in-flight ops per io_uring ring).
    /// 0 (default) means "use threadsPerDisk" (legacy alias). Set > 0 to
    /// decouple pipeline depth from physical thread count -- matches
    /// Hypernode's model where a modest number of iocore threads each hold
    /// thousands of in-flight ops via a deep ring.
    int queueDepth{0};
    /// Number of io_uring reactor THREADS spawned per mount point. Default 1
    /// matches historical halcyon (1:1 reactor:mount). Hypernode uses ~64
    /// iocore threads for 16 SSDs (4:1). Setting > 1 spawns that many
    /// independent reactor threads per mount, each with its own io_uring
    /// ring, fds, OpSlots, and PerfStats row -- all sharing the underlying
    /// mount's data files (kernel handles concurrent-fd read/write). Total
    /// reactor threads = mountPoints * reactorsPerMount. Currently
    /// **manifest backend only** -- rocksdb needs per-mount ChunkMapper
    /// sharing / concurrency work that isn't done here.
    int reactorsPerMount{1};
    /// Stats sink; rows must be sized [mountPoints *
    /// reactorsPerMount][queueDepth]. Owned by the caller (the benchmark driver
    /// in B7/B8).
    PerfStats* perfStats{nullptr};
  };

  IoUringEngine(HalcyonPools& pools, Config config);

  /// Spawns one IO-Core reactor per mount point, runs each until
  /// warmupSec + runtimeSec has elapsed, then joins them.
  void run();

 private:
  HalcyonPools& pools_;
  Config config_;
};

} // namespace facebook::halcyon
