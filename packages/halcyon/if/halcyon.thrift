include "thrift/annotation/cpp.thrift"
include "thrift/annotation/thrift.thrift"

// @lint-ignore THRIFTCHECKS thrift-parser -- preserve the existing wire schema identity
@thrift.AllowLegacyMissingUris
package;

namespace cpp2 cea.halcyon
namespace py cea.halcyon.py3
namespace py3 cea.halcyon.py3

@cpp.Type{name = "folly::IOBuf"}
typedef binary IOBuf
@cpp.Type{name = "std::unique_ptr<folly::IOBuf>"}
typedef binary IOBufPtr

const i32 kDefaultPort = 23459;
const i32 kMaxFileSize = 8462336;
const i32 kBufferSize = 8388608;
const i32 kChunkSize = 8388608;
const i32 kFragmentSize = 4096;
const i32 kChunkHeaderSize = 4096;
const i32 kMaxDataFragmentSize = 8458240;
const i32 kChecksumSize = 4096;

enum HalcyonReturnCode {
  Success = 0,
  DefaultError = 1,
  ConfigurationError = 2,
}

struct HalcyonResponse {
  1: HalcyonReturnCode return_code;
  2: string message;
}

exception HalcyonException {
  1: HalcyonReturnCode code;
  2: string message;
}

// Only simple CRUD like operations are supported, as the benchmark is intends
// to mimic distributed filesystems used in Big Data environments.
enum Operation {
  Read = 1,
  Write = 2,
  Append = 3,
  Create = 4,
  Delete = 5,
}

// A node can be paired with another to transfer network I/O.  One node sends
// data based on the read distribution, and the other based on the write
// distribution.  In this way a workload that is 100% read or write will
// only use one direction, just like production.
enum PairRole {
  SendNone = 0,
  SendReads = 1,
  SendWrites = 2,
  SendAll = 3,
  SendHalf = 4,
  // Disk-derived paired mode: the reactor forwards each completed read's
  // buffer to the network layer, which sends those actual bytes to the
  // partner instead of synthetic RAM-buffer contents. Couples file_xput to
  // net_xput so NIC saturation naturally back-pressures the storage stack.
  // Reads are forwarded; writes go direct-to-disk as usual. Requires the
  // partner to also run halcyon (partner-side just receives and discards).
  SendDiskReads = 5,
  // Fully-coupled paired mode: like SendDiskReads for reads, but also
  // for writes -- the sender's write payload is sent over the network to
  // the partner, and the partner submits an actual disk write with the
  // received buffer instead of generating write data locally from RAM.
  // Both directions of the pair traffic real read/write bytes (each op
  // tagged via HalcyonRequest.op = Read/Write). Removes the write-path
  // measurement asymmetry when comparing with Hypernode (where writes
  // arrive from clients over the network). Requires the partner to run
  // halcyon with the same mode.
  SendDiskBoth = 6,
  // Client/server RPC-driven mode -- mirrors Hypernode's fbfio access
  // pattern. Client side (readRatio + writeRatio drive the mix) generates
  // ChunkOpRequest RPCs: a putChunk request sends a 1 MB payload to the
  // server, which pwrites it to disk and returns an ack; a readChunk
  // request sends a small id + length and the server responds with the
  // requested chunk data. Server side owns all disk state (uses
  // HalcyonChunkMapper for id -> location); client side owns no local
  // storage state. Unlike SendDiskBoth this is a true one-request-one-
  // response client/server split, matching HN's Thrift RPC surface, and
  // enables direct halcyon <-> fbfio latency + memBW comparison.
  RequestResponse = 7,
}

// A node does not need to have a partner to send network I/O.  In that case
// the benchmark will use the loopback interface and sample 50% of the
// requests.
struct NodeConfig {
  1: string name = 'localhost';
  2: string partner;
  3: PairRole role;
  4: list<string> mountpoints;
}

// The benchmark assumes a regular, fairly flat layout.  At the root of each
// mountpoint there are a fixed number of directories, each with a fixed
// number of subdirectories.  In each subdirectory there are a fixed number
// of files.
struct FileLayout {
  1: i64 maxFileSize;
  2: i32 dirs;
  3: i32 subdirs;
  4: i32 filesPerDir;
}

struct IoSizeRange {
  1: string text;
  2: i64 lowerBound;
  3: i64 upperBound;
  4: double percentage;
}

/*
                       +-------------+
                       |Chunk Diagram|
                       +-------------+
            +----------+              +----------+
            |  header  |              |  header  |
            +----------+              +----------+
            +----------+              +----------+
            |          |              |          |
            |          |              |          |
            | fragment |              | fragment |
            |          |              | +--------+
            |          |    inclusive | |checksum|
            +----------+              +-+--------+
  exclusive | checksum |              +----------+
            +----------+              |          |
            +----------+              |          |
            |          |              | fragment |
            |          |              | +--------+
            | fragment |              | |checksum|
            |          |              +-+--------+
            |          |                   ...
            +----------+              +----------+
            | checksum |              |  footer  |
            +----------+              +----------+
                ...
            +----------+
            |  footer  |
            +----------+
*/

struct ChunkConfiguration {
  1: i32 headerSize;
  2: i32 fragmentSize;
  3: i32 numFragments;
  4: i32 footerSize;
  5: i32 checksumSize;
  6: bool checksumInclusive = true;
  7: bool checksumOnRead = false;
}

struct BenchmarkConfiguration {
  2: double readPercentage;
  3: list<IoSizeRange> readSizes;
  4: list<IoSizeRange> writeSizes;
  9: list<IoSizeRange> mergedSizes;
  5: list<string> mountpoints;
  6: FileLayout fileLayout;
  7: i32 threadsPerDisk = 5;
  8: RuntimeConfiguration runtimeConfig;
  10: NodeConfig nodeConfig;
}

// Selects how the benchmark addresses I/O: the legacy manifest + flat-grid path,
// or the RocksDB chunk-mapper path (mirrors facebook::halcyon::MetadataBackend).
enum MetadataBackend {
  Manifest = 0,
  RocksDb = 1,
}

struct RuntimeConfiguration {
  1: i32 warmupTime;
  2: i32 runTime;
  3: i32 reportingInterval;
  4: double qps;
  5: i32 threadsPerDisk;
  10: i32 networkThreads;
  6: i32 numMountpoints;
  11: bool doDirectIo;
  7: bool doFileIo;
  8: bool doNetworkIo;
  9: bool doCacheIo;
  12: MetadataBackend metadataBackend = MetadataBackend.Manifest;
  // QFlush write-offload pool size (rocksdb backend only). 0 = inline chunk-map
  // puts on the reactor thread (no offload); > 0 spawns that many FlushWorkers
  // so the put + WAL-sync cost moves to the QFlush pool. Mirrors the standalone
  // binary's --qflush_pool_size.
  13: i32 qflushThreads = 0;
  // Per-reactor ChunkMetadataCache capacity in entries (rocksdb backend only).
  // 0 = no read cache; > 0 builds an LRU of that many chunk-id -> location
  // entries so hot reads skip the DB lookup. Mirrors --cache_enabled /
  // --cache_capacity on the standalone binary.
  14: i32 cacheCapacity = 0;
  // Read-key access locality probability in [0,1] (rocksdb backend). With this
  // probability a read targets the most-recent `cacheWindow` chunk ids, so the
  // cache hit rate emerges from reuse; 0 = uniform key selection.
  15: double cacheLocality = 0.0;
  // Hot-window size in chunk ids for cacheLocality; 0 = no locality window.
  16: i32 cacheWindow = 0;
  // Pin pool threads to specific CPU cores when true. Mirrors the standalone
  // binary's --pin_threads.
  17: bool pinThreads = false;
  // Number of cores to allocate to the QIOThread pool. When pinThreads=true and
  // this > 0, the pool is pinned to cores [0, qioCoresCount). 0 disables QIO
  // pinning even if pinThreads is set.
  18: i32 qioCoresCount = 0;
  // Number of cores to allocate to the QFlush pool. When pinThreads=true and
  // this > 0, the pool is pinned to cores [qioCoresCount, qioCoresCount +
  // qflushCoresCount) so it doesn't overlap the QIO pool. 0 disables QFlush
  // pinning.
  19: i32 qflushCoresCount = 0;
  // Deprecated no-op, retained for wire compatibility: NetworkOperator
  // always opens a direct plaintext RocketClientChannel to the partner
  // daemon.
  20: bool pairPlaintext = false;
  // io_uring submission/completion ring entries per reactor. Halcyon has
  // historically hard-coded 256; Hypernode's iocore uses up to 32768
  // (kMaxIOUringRingEntries). Deeper rings let each reactor hold more
  // in-flight ops -- important when --queue-depth raises the per-reactor
  // pipeline depth. Auto-clamped up to max(threadsPerDisk, this value) so a
  // ring never bottlenecks a high --threads-per-disk. 0 keeps the legacy
  // 256 default.
  22: i32 ringEntries = 0;
  // Number of OpSlots per reactor: max concurrent in-flight I/Os on ONE
  // io_uring ring. Legacy default was to alias this to threadsPerDisk,
  // conflating "how many physical worker threads" with "how deep is each
  // reactor's pipeline". Hypernode's iocore keeps thread count modest
  // (~64/node) but pushes per-thread in-flight ops into the thousands via
  // deep rings. Setting this > 0 decouples the two: each reactor still
  // uses one physical thread from the QIO pool, but allocates `queueDepth`
  // OpSlots and juggles them via io_uring completion polling. 0 = legacy
  // (opSlots = threadsPerDisk). Warning: each OpSlot pre-allocates a
  // kMaxFileSize (~8.46 MB) buffer, so memory = queueDepth * mounts *
  // 8.46 MB. 500 * 14 mounts = ~59 GB per host on our T8_SRF setup.
  23: i32 queueDepth = 0;
  // Convert all data-path worker pools (NetworkOperator, FlushWorker,
  // MetadataWorker) from event-driven (sleep-when-idle) to busy-poll
  // (pause-when-idle). Matches Hypernode's ThreadMgr::threadWorker pattern
  // (fbcode/hypernode/util/ThreadMgr.cpp:811) where iocore, dptworker,
  // chunkMapper, and jmc all run tight `while (state==Init) { loopCb();
  // pause; pause; }` loops -- 100% CPU per thread even when idle. This is
  // what accounts for Hypernode's ~70% baseline CPU util from ~120 spinning
  // threads on T8. Halcyon's default event-driven model gets ~10% baseline
  // from just the 14 reactor threads spinning. When true, expect QNet /
  // QFlush / QMetadata pools to pin at 100% CPU per thread -- users should
  // typically also raise --qnet_pool_size on the daemon to match
  // --network-threads so every coroutine gets its own dedicated thread.
  24: bool busyPollDataPath = false;
  // Experimental: number of io_uring reactor THREADS spawned per mount.
  // Default 1 keeps 1:1 reactor:mount (historical halcyon). Set to N to
  // spawn N reactors per mount, each with its own io_uring ring + fds +
  // OpSlots. Total reactor threads = mountPoints * reactorsPerMount. Adds
  // ~N-1 more busy-poll threads per mount => big cpu_util lift. Currently
  // manifest-only: throws at run() start if backend == RocksDb, because
  // multiple reactors on one mount would race on the same chunkmap.rocksdb
  // path (rocksdb dir lock + shared HalcyonChunkMapper concurrency work
  // not done here).
  25: i32 reactorsPerMount = 1;
  // Number of MetadataWorker (chunk-map READ) instances to spawn on the
  // QMetadata pool. 0 (default) keeps inline lookups on the reactor
  // thread. > 0 offloads reads to the QMetadata pool -- pairs with
  // --qflush-threads to split chunk-map work into separate read + write
  // pools, matching Hypernode's chunkMapper thread accounting. Requires
  // daemon --qmeta_pool_size >= this value for full concurrency.
  26: i32 qmetaThreads = 0;
  // Spawn 1 pinned idle-spin thread per otherwise-unused core so halcyon's
  // per-core CPU utilization matches Hypernode's "one perpetually spinning
  // thread per physical core" shape. Each idle-spin thread runs a tight
  // `folly::asm_volatile_pause` loop pinned to a specific core, does no
  // useful work, and is joined at benchmark teardown. When true and
  // pinThreads=true, auto-pin computes idle_cores as the gap between the
  // reactor range and the qflush range, and unpins qioPool workers so they
  // do not fight the spin threads for their cores. Off by default -- the
  // spin threads are pure benchmark-parity, not throughput-relevant.
  27: bool fillIdleCores = false;
}

struct IoStats {
  1: i64 readIos;
  2: i64 writeIos;
  3: i64 readBytes;
  4: i64 writeBytes;
  5: i64 readUsec;
  6: i64 writeUsec;
  // Hypernode-style reactor-loop CPU accounting. usefulBusyNs is the sum of
  // reactor-loop iterations that did real I/O work, minus a rolling average of
  // idle-loop overhead. usefulIdleNs is the sum of idle iterations plus the
  // subtracted overhead. The ratio busy/(busy+idle) is the reactor's "useful
  // CPU" -- unlike Linux mpstat which reports a busy-poll core at 100%
  // regardless of whether it's actually doing work. Populated per-reactor on
  // slot 0.
  7: i64 usefulBusyNs = 0;
  8: i64 usefulIdleNs = 0;
}

struct CreateStats {
  1: i64 files;
  2: i64 dirs;
  3: i64 start;
  4: i64 end;
  5: i64 bytes;
  6: i32 threadsComplete;
}

struct ChecksumStats {
  1: i64 readCount;
  2: i64 writeCount;
  3: i64 readNanos;
  4: i64 writeNanos;
}

struct PerfStats {
  1: i32 numOperators;
  2: i32 numWorkers;
  3: list<list<IoStats>> istats;
  4: list<list<ChecksumStats>> cstats;
}

struct NetworkStats {
  1: i64 sendIos = 0;
  2: i64 recvIos = 0;
  3: i64 sendBytes = 0;
  4: i64 recvBytes = 0;
  5: i64 sendUsec = 0;
  6: i64 recvUsec = 0;
  7: i32 numOperators = 0;
}

struct CpuStats {
  1: i64 ts = 0;
  2: i64 user = 0;
  3: i64 nice = 0;
  4: i64 system = 0;
  5: i64 idle = 0;
  6: i64 iowait = 0;
  7: i32 nCpu = 0;
  8: i32 ticksPerSec = 0;
}

struct HalcyonRequest {
  1: i32 size;
  2: IOBufPtr payload;
  3: Operation op;
}

// RequestResponse pair-role RPC. `op` decides the direction:
//   - Read : `size` is requested bytes; `chunkId` names the target; server
//     responds with ChunkOpResponse.payload = chunk data of that size.
//   - Write: `payload` holds the bytes to store; server pwrites and returns
//     empty payload. `chunkId` = 0 means server auto-allocates a fresh id
//     (returned in ChunkOpResponse.chunkId).
@thrift.Uri{value = "meta.com/cea/halcyon/ChunkOpRequest"}
struct ChunkOpRequest {
  1: Operation op;
  2: i64 chunkId;
  3: i32 size;
  4: IOBufPtr payload;
}

@thrift.Uri{value = "meta.com/cea/halcyon/ChunkOpResponse"}
struct ChunkOpResponse {
  1: HalcyonReturnCode return_code;
  2: i64 chunkId;
  3: IOBufPtr payload;
}

service HalcyonService {
  // Partner node methods
  HalcyonResponse setPartner(1: string hostname);
  string getPartner();
  HalcyonResponse setPairRole(1: PairRole role);
  PairRole getPairRole();
  HalcyonResponse sendData(1: HalcyonRequest data) throws (
    1: HalcyonException e,
  );

  // RequestResponse (RPC client/server) mode. Client's reactors issue
  // chunkOp() calls; server routes to its io_uring reactor for real disk I/O
  // and returns the result. Analog of Hypernode's readChunk / putChunk RPCs.
  ChunkOpResponse chunkOp(1: ChunkOpRequest req) throws (1: HalcyonException e);

  // Fileset creation methods
  HalcyonResponse setFilesetLayout(1: FileLayout layout);
  HalcyonResponse startFilesetCreation();
  CreateStats getFilesetCreationStats();
  bool isFilesetCreationRunning();
  double getFilesetCreationProgress();
  void stopFilesetCreation();

  // Benchmark execution methods
  HalcyonResponse setBenchmarkConfiguration(1: BenchmarkConfiguration config);
  HalcyonResponse startBenchmark();
  bool isBenchmarkRunning();
  void stopBenchmark();

  // Performance monitoring methods
  i32 getLinkSpeed();
  double getDiskUtilization();
  double getCpuUtilization();
  double getNetworkThroughput();
  double getDiskUtilizationFromBeginning();
  double getCpuUtilizationFromBeginning();
  double getNetworkThroughputFromBeginning();
  PerfStats getPerfStats();
  NetworkStats getNetworkStats();
  CpuStats getCpuStats();
}
