// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>

#include <folly/Format.h>
#include <folly/coro/Task.h>
#include <folly/hash/Checksum.h>
#include <folly/hash/FnvHash.h>
#include <folly/hash/SpookyHashV2.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/Asm.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "Common.h"
#include "Fileset.h"
#include "IoUringEngine.h"
#include "NetworkOperator.h"
#include "ThreadPools.h"
#include "cpp2/HalcyonServiceHandler.h"

// --- RequestResponse / HN-workload emulation knobs --------------------------
DEFINE_int64(
    rr_chunk_id_range,
    100000,
    "RequestResponse mode: upper bound (exclusive) of the random chunkId the "
    "client picks per RPC. Must be <= the number of chunks the partner's "
    "chunkmap.rocksdb covers, else out-of-range ids return empty payloads and "
    "throughput drops proportionally.");

DEFINE_bool(
    rr_hn_workload,
    false,
    "RequestResponse mode: add HN-shaped per-RPC overhead (CRC32C over the "
    "payload + per-op stats bookkeeping) to close the uArch MPKI gap vs "
    "Hypernode. Set 0 to see raw halcyon efficiency.");

DEFINE_int32(
    rr_hn_workload_multiplier,
    1,
    "RequestResponse mode: number of CRC32C(+hash) passes over the payload per "
    "RPC. N>1 over-simulates to hit HN's per-instruction miss density. Only "
    "active when --rr_hn_workload.");

DEFINE_int32(
    rr_pool_buffers,
    1024,
    "IoBufferPool size (RR read path). Smaller values narrow the pool working "
    "set to fit L2/LLC, reducing per-RPC cache misses.");

DEFINE_bool(
    rr_hn_multihash,
    true,
    "When true, the HN workload runs CRC32C + FNV-64 + SpookyHashV2 per pass; "
    "when false only CRC32C. Use 0 on E-core SKUs, then raise the multiplier.");

DEFINE_int32(
    rr_hn_compute_iters,
    0,
    "RequestResponse mode: per-RPC register-only compute iterations. Dials up "
    "instructions/byte without cache pressure (lowers MPKI-per-instruction). "
    "Represents HN per-op state-machine work halcyon lacks.");

using namespace std;
using namespace folly;
using namespace folly::coro;
using namespace cea::halcyon;

namespace facebook {

namespace {
// Maps the thrift MetadataBackend enum to the engine's C++ enum.
halcyon::MetadataBackend toEngineBackend(cea::halcyon::MetadataBackend b) {
  return b == cea::halcyon::MetadataBackend::RocksDb
      ? halcyon::MetadataBackend::RocksDb
      : halcyon::MetadataBackend::Manifest;
}

// Returns the HT sibling of `cpu` (the other logical CPU sharing its physical
// core), or -1 if the topology can't be read or SMT is disabled. Reads
// /sys/devices/system/cpu/cpuN/topology/thread_siblings_list, whose format
// is either "N,M" or "N-M" for two-thread cores.
int htSibling(int cpu) {
  auto path = folly::sformat(
      "/sys/devices/system/cpu/cpu{}/topology/thread_siblings_list", cpu);
  std::ifstream f(path);
  if (!f) {
    return -1;
  }
  std::string line;
  std::getline(f, line);
  int a = -1, b = -1;
  char sep = 0;
  if (std::sscanf(line.c_str(), "%d%c%d", &a, &sep, &b) == 3) {
    return (a == cpu) ? b : a;
  }
  return -1;
}
} // namespace

HalcyonServiceHandler::HalcyonServiceHandler(
    std::shared_ptr<halcyon::HalcyonPools> processPools)
    : processPools_(std::move(processPools)) {
  LOG(INFO) << "Initializing server...";
  CHECK(processPools_ != nullptr) << "processPools must be provided";
  partner_ = "localhost";
  transactions_ = 0;
  bytes_ = 0;
  pleaseStop = false;
  readPercentage_ = 100;
  threadsPerDisk_ = 5;
  networkThreads_ = 16;
  qflushThreads_ = 0;
  cacheCapacity_ = 0;
  cacheLocality_ = 0.0;
  cacheWindow_ = 0;
  runtime_ = 300;
  warmup_ = 120;
  reportingInterval_ = 30;
  qps_ = 0;
  readPercentage_ = 100;
  role_ = PairRole::SendHalf;
  doFileIo_ = true;
  doNetworkIo_ = true;
  doCacheIo_ = true;
}

Task<unique_ptr<HalcyonResponse>> HalcyonServiceHandler::co_setPartner(
    unique_ptr<string> hostname) {
  LOG(INFO) << "got setPartner thrift call";
  partner_ = *hostname.get();
  LOG(INFO) << sformat("partner hostname is {}", partner_);
  auto resp = std::make_unique<HalcyonResponse>();
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->message() = "partner set successfully";
  co_return resp;
}

Task<unique_ptr<string>> HalcyonServiceHandler::co_getPartner() {
  auto resp = std::make_unique<string>(partner_);
  co_return resp;
}

Task<unique_ptr<HalcyonResponse>> HalcyonServiceHandler::co_setPairRole(
    PairRole role) {
  LOG(INFO) << "got setPairRole thrift call";
  role_ = role;
  auto resp = std::make_unique<HalcyonResponse>();
  resp->return_code() = HalcyonReturnCode::Success;
  resp->message() = "partner role set successfully";
  co_return resp;
}

Task<PairRole> HalcyonServiceHandler::co_getPairRole() {
  co_return role_;
}

namespace {
// Reads a "dirs files" manifest under mountpoint. Returns pair(dirs, files).
// Throws on any parse/open failure so setup fails loudly.
std::pair<int, int> readFilesetManifest(const std::string& mountpoint) {
  const std::string path = mountpoint + "/manifest";
  std::ifstream f(path);
  if (!f) {
    throw std::runtime_error(sformat("no manifest at {}", path));
  }
  int dirs = 0, files = 0;
  f >> dirs >> files;
  if (dirs <= 0 || files <= 0) {
    throw std::runtime_error(
        sformat("manifest {} has bogus dirs={} files={}", path, dirs, files));
  }
  return {dirs, files};
}
} // namespace

// Initializes server-side state for PairRole::RequestResponse:
// one HalcyonChunkMapper + open fileset FDs per mount, so co_chunkOp can
// serve reads with a plain pread from the QNet coroutine.
// Called once per setBenchmarkConfiguration when RR role is active.
void HalcyonServiceHandler::initRequestResponseState() {
  if (role_ != PairRole::RequestResponse) {
    return;
  }
  // Do NOT re-init if the state is already populated. Mountpoints don't
  // change between benchmark configs on the same daemon, and re-init would
  // race with in-flight chunkOp handlers holding refs into rrMappers_ /
  // rrFds_ (dropping the ChunkMapper closes the underlying RocksDB, and any
  // concurrent mapper->lookup() would UAF). Persistent state across runs
  // is fine -- chunkmap + fileset FDs are read-only from the handler's view.
  if (!rrMappers_.empty()) {
    LOG(INFO) << "RequestResponse state already initialized ("
              << rrMappers_.size() << " mappers) -- skipping re-init";
    return;
  }
  const size_t n = mountpoints_.size();
  rrMappers_.reserve(n);
  rrFds_.reserve(n);
  for (const auto& mp : mountpoints_) {
    // One ChunkMapper per mount, sharing a single 10 GiB block cache so hot
    // reads across mounts don't fragment the LRU (mirrors HN behaviour).
    // First iteration: no shared cache -- each mapper's own cache. This can
    // migrate to the shared-cache pattern already used in IoUringEngine.
    halcyon::HalcyonChunkMapper::Options opts;
    opts.doDirectReads = false;
    rrMappers_.push_back(
        std::make_shared<halcyon::HalcyonChunkMapper>(
            mp + "/chunkmap.rocksdb", opts));

    auto [dirs, files] = readFilesetManifest(mp);
    const int numFiles = dirs * files;
    std::vector<int> fds;
    fds.reserve(numFiles);
    for (int i = 0; i < numFiles; ++i) {
      const int d = i / files;
      const int f = i - d * files;
      const std::string path = sformat("{}/d{}/f{}", mp, d, f);
      // Read-only, no O_DIRECT: pread returns page-cached data straight to a
      // malloc'd buffer we hand to IOBuf. First-cut correctness > speed.
      const int fd = ::open(path.c_str(), O_RDONLY);
      if (fd < 0) {
        for (int f2 : fds) {
          ::close(f2);
        }
        throw std::runtime_error(
            sformat("RR init: open {} failed (errno {})", path, errno));
      }
      fds.push_back(fd);
    }
    rrFds_.push_back(std::move(fds));
  }
  LOG(INFO) << sformat(
      "RequestResponse server state: {} mounts, {} total files opened",
      rrMappers_.size(),
      std::accumulate(
          rrFds_.begin(), rrFds_.end(), size_t{0}, [](size_t a, const auto& v) {
            return a + v.size();
          }));
}

folly::coro::Task<std::unique_ptr<ChunkOpResponse>>
HalcyonServiceHandler::co_chunkOp(std::unique_ptr<ChunkOpRequest> req) {
  auto resp = std::make_unique<ChunkOpResponse>();
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->chunkId() = *req->chunkId();
  *resp->payload() = folly::IOBuf::create(0);

  // If RR state isn't initialized, return the stub-success (wire test only).
  if (rrMappers_.empty()) {
    transactions_++;
    co_return resp;
  }

  const size_t mIdx = static_cast<size_t>(*req->chunkId()) % rrMappers_.size();
  auto& mapper = rrMappers_[mIdx];
  auto& fds = rrFds_[mIdx];

  if (*req->op() == cea::halcyon::Operation::Read) {
    halcyon::ChunkLocation loc;
    if (!mapper->lookup(static_cast<halcyon::ChunkId>(*req->chunkId()), loc)) {
      *resp->return_code() = HalcyonReturnCode::DefaultError;
      transactions_++;
      co_return resp;
    }
    if (loc.fileIndex >= fds.size()) {
      *resp->return_code() = HalcyonReturnCode::DefaultError;
      transactions_++;
      co_return resp;
    }
    // Serve at most what the request asked for, bounded by the on-disk
    // chunk length.
    const size_t want =
        std::min<size_t>(static_cast<size_t>(*req->size()), loc.length);
    // pread straight into an owning IOBuf instead of filling a std::vector and
    // handing it to copyBuffer. That cost 2 heap allocations of `want` plus a
    // full memcpy per read op (vector zero-init, pread fill, then the IOBuf
    // copy); now it is 1 allocation and no copy. append() runs only after
    // `got` is known, which is what the earlier copyBuffer comment here was
    // guarding against -- the length metadata is never left unset.
    auto iobuf = folly::IOBuf::create(want);
    // create() either returns a live buffer or throws bad_alloc, so bind a
    // reference once instead of dereferencing the unique_ptr three times
    // through the hot path.
    folly::IOBuf& payload = *iobuf;
    const ssize_t got =
        ::pread(fds[loc.fileIndex], payload.writableData(), want, loc.offset);
    if (got < 0) {
      *resp->return_code() = HalcyonReturnCode::DefaultError;
      transactions_++;
      co_return resp;
    }
    payload.append(static_cast<size_t>(got));
    // Kept live for the HN-workload passes below: moving the unique_ptr
    // transfers ownership to resp but does not relocate the payload bytes.
    const uint8_t* const srcData = payload.data();
    const size_t srcLen = static_cast<size_t>(got);
    *resp->payload() = std::move(iobuf);
    bytes_ += got;
    // HN-workload emulation (read side): CRC32C(+multihash) passes over the
    // payload + a register-only compute loop + scattered atomic stat bumps.
    if (FLAGS_rr_hn_workload) {
      volatile uint32_t crc = 0;
      volatile uint64_t xh = 0;
      volatile uint64_t sh = 0;
      for (int i = 0; i < FLAGS_rr_hn_workload_multiplier; ++i) {
        crc = folly::crc32c(srcData, srcLen, crc);
        if (FLAGS_rr_hn_multihash) {
          xh ^= folly::hash::fnv64_buf(srcData, srcLen);
          sh ^= folly::hash::SpookyHashV2::Hash64(srcData, srcLen, sh);
        }
      }
      (void)crc;
      (void)xh;
      (void)sh;
      volatile uint64_t work = static_cast<uint64_t>(*req->chunkId()) ^
          (static_cast<uint64_t>(got) << 20);
      for (int i = 0; i < FLAGS_rr_hn_compute_iters; ++i) {
        work = work * 0x9e3779b185ebca87ULL + 0xdeadbeefcafebabeULL;
        work ^= work >> 27;
      }
      (void)work;
      hnStatsCounter_.fetch_add(1, std::memory_order_relaxed);
      hnStatsBytes_.fetch_add(
          static_cast<uint64_t>(got), std::memory_order_relaxed);
      hnStatsLatencyBucket_[static_cast<size_t>(got) % kHnStatsBuckets]
          .fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    // Write path: land in the next iteration. For now the payload is
    // consumed but not written to disk; we ack success so the wire keeps
    // working end-to-end for RPC benchmarking.
    // Use *req->size() (already deserialized as an int) instead of walking
    // the IOBuf chain for length -- computeChainDataLength was 4.5% of CPU
    // in strobelight before this shortcut.
    bytes_ += static_cast<uint64_t>(*req->size());
    // HN-workload emulation (write side): mirror the read path over the
    // request payload so writes carry the same per-op inst/cache density.
    if (FLAGS_rr_hn_workload) {
      const uint8_t* src = nullptr;
      size_t src_len = 0;
      if (req->payload()->get() != nullptr) {
        const auto br = req->payload()->get()->coalesce();
        src = br.data();
        src_len = br.size();
      }
      volatile uint32_t crc = 0;
      volatile uint64_t xh = 0;
      volatile uint64_t sh = 0;
      for (int i = 0; i < FLAGS_rr_hn_workload_multiplier; ++i) {
        if (src_len) {
          crc = folly::crc32c(src, src_len, crc);
          if (FLAGS_rr_hn_multihash) {
            xh ^= folly::hash::fnv64_buf(src, src_len);
            sh ^= folly::hash::SpookyHashV2::Hash64(src, src_len, sh);
          }
        }
      }
      (void)crc;
      (void)xh;
      (void)sh;
      volatile uint64_t work = static_cast<uint64_t>(*req->chunkId()) ^
          (static_cast<uint64_t>(*req->size()) << 20);
      for (int i = 0; i < FLAGS_rr_hn_compute_iters; ++i) {
        work = work * 0x9e3779b185ebca87ULL + 0xdeadbeefcafebabeULL;
        work ^= work >> 27;
      }
      (void)work;
      hnStatsCounter_.fetch_add(1, std::memory_order_relaxed);
      hnStatsBytes_.fetch_add(
          static_cast<uint64_t>(*req->size()), std::memory_order_relaxed);
      hnStatsLatencyBucket_[static_cast<size_t>(*req->size()) % kHnStatsBuckets]
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  transactions_++;
  co_return resp;
}

folly::coro::Task<std::unique_ptr<HalcyonResponse>>
HalcyonServiceHandler::co_sendData(std::unique_ptr<HalcyonRequest> request) {
  auto resp = std::make_unique<HalcyonResponse>();
  folly::IOBuf& srcData = *request->payload()->get();
  ssize_t bytesTransferred = srcData.computeChainDataLength();
  const auto op = *request->op();

  // SendDiskBoth mode: if the sender tagged this as a Write, move the
  // Thrift-received IOBuf chain directly into the RecvItem -- no
  // intermediate `new uint8_t[N]` + Cursor::pull here. The reactor pulls
  // the chain into its aligned O_DIRECT pool buffer inside
  // submitRecvWrite, so we replace two memcpys (chain -> heap buf ->
  // aligned buf) with one (chain -> aligned buf directly). Matches HN's
  // ThriftDpt behaviour of holding the received IOBuf through to the
  // io_uring submit. blockingWrite still back-pressures the Thrift server
  // when the reactor can't keep up.
  if (recvQueue_ != nullptr && op == cea::halcyon::Operation::Write) {
    halcyon::RecvItem item{std::move(*request->payload())};
    recvQueue_->blockingWrite(std::move(item));
    bytes_ += bytesTransferred;
    transactions_++;
    *resp->return_code() = HalcyonReturnCode::Success;
    *resp->message() = "";
    co_return resp;
  }

  // Discard path (Reads in every paired mode, plus both directions in the
  // pre-SendDiskBoth modes). Nothing downstream consumes these bytes -- the
  // payload was already DMA'd into DRAM by the NIC. The prior code did a
  // full `new + Cursor::pull + free` here, burning ~48 GB/s of DRAM per
  // direction at 24 GB/s network throughput for a buffer that was
  // immediately freed. `computeChainDataLength` walked the chain in O(1)
  // (cached lengths) above; just account and drop, letting the IOBuf chain
  // destruct as request goes out of scope. Matches Hypernode, which never
  // touches bytes it isn't going to persist or forward.
  bytes_ += bytesTransferred;
  transactions_++;
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->message() = "";
  co_return resp;
}

Task<unique_ptr<HalcyonResponse>> HalcyonServiceHandler::co_setFilesetLayout(
    unique_ptr<FileLayout> layout) {
  auto resp = make_unique<HalcyonResponse>();
  co_return resp;
}

Task<unique_ptr<HalcyonResponse>>
HalcyonServiceHandler::co_startFilesetCreation() {
  auto resp = make_unique<HalcyonResponse>();
  int n = mountpoints_.size();
  auto createStats = std::make_shared<halcyon::CreateStats>(n, threadsPerDisk_);
  for (int i = 0; i < n; i++) {
    // Each worker captures a strong reference, so the stats object stays alive
    // for the worker's whole lifetime even if a later startFilesetCreation
    // swaps createStats_ out.
    // Snapshot every argument at launch time, matching the previous
    // async(&launchFilesetCreation, ...) semantics where each argument was
    // copied when the task was queued rather than read on the worker thread.
    auto mountpoint = mountpoints_.at(i);
    auto dirs = *fileLayout_.dirs();
    auto filesPerDir = *fileLayout_.filesPerDir();
    auto threadsPerDisk = threadsPerDisk_;
    auto backend = backend_;
    auto readSizes = readSizes_;
    cfuts_.push_back(async(
        launch::async,
        [i,
         mountpoint,
         dirs,
         filesPerDir,
         threadsPerDisk,
         backend,
         readSizes,
         createStats]() {
          return halcyon::launchFilesetCreation(
              i,
              threadsPerDisk,
              mountpoint,
              dirs,
              filesPerDir,
              createStats.get(),
              backend,
              readSizes);
        }));
    LOG(INFO) << mountpoints_.at(i);
  }
  {
    std::lock_guard<std::mutex> guard(createStatsMu_);
    createStats_ = std::move(createStats);
  }
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->message() = "Successfully started fileset creation";
  co_return resp;
};

std::shared_ptr<halcyon::CreateStats>
HalcyonServiceHandler::snapshotCreateStats() const {
  std::lock_guard<std::mutex> guard(createStatsMu_);
  return createStats_;
}

Task<unique_ptr<CreateStats>>
HalcyonServiceHandler::co_getFilesetCreationStats() {
  auto resp = make_unique<CreateStats>();
  auto stats = snapshotCreateStats();
  if (stats == nullptr) {
    co_return resp;
  }
  uint64_t min_start = halcyon::current_nano();
  uint64_t max_end = 0;
  for (const auto& i : stats->stats) {
    for (auto j : i) {
      *resp->files() += j.files;
      *resp->dirs() += j.dirs;
      *resp->bytes() += j.bytes;
      min_start = j.start < min_start ? j.start : min_start;
      max_end = j.end > max_end ? j.end : max_end;
    }
  }
  *resp->start() = min_start;
  *resp->end() = max_end;
  co_return resp;
}

bool HalcyonServiceHandler::isFilesetCreationRunning() {
  auto stats = snapshotCreateStats();
  return stats != nullptr && stats->inProgress;
};

double HalcyonServiceHandler::getFilesetCreationProgress() {
  auto stats = snapshotCreateStats();
  if (stats == nullptr) {
    return 0.0;
  }
  int n = mountpoints_.size();
  int dirs = *fileLayout_.dirs();
  int files = *fileLayout_.filesPerDir();
  int totalFiles = n * dirs * files * threadsPerDisk_;
  int currFiles = 0;
  for (const auto& i : stats->stats) {
    for (auto j : i) {
      currFiles += j.files;
    }
  }
  return 100 * (static_cast<double>(currFiles)) / totalFiles;
};

void HalcyonServiceHandler::stopFilesetCreation() {
  pleaseStop = true;
};

// Benchmark execution methods
Task<unique_ptr<HalcyonResponse>>
HalcyonServiceHandler::co_setBenchmarkConfiguration(
    unique_ptr<BenchmarkConfiguration> config) {
  LOG(INFO) << "got setBenchmarkConfiguration thrift call";
  benchmarkConfig_ = config.get();
  readPercentage_ = *benchmarkConfig_->readPercentage();
  readSizes_.clear();
  auto readSizes = *benchmarkConfig_->readSizes();
  for (auto v : readSizes) {
    halcyon::IoSizeRange readRange;
    readRange.text = *v.text();
    readRange.lowerBound = *v.lowerBound();
    readRange.upperBound = *v.upperBound();
    readRange.percentage = *v.percentage();
    readSizes_.push_back(readRange);
  }
  writeSizes_.clear();
  auto writeSizes = *benchmarkConfig_->writeSizes();
  for (auto v : writeSizes) {
    halcyon::IoSizeRange writeRange;
    writeRange.text = *v.text();
    writeRange.lowerBound = *v.lowerBound();
    writeRange.upperBound = *v.upperBound();
    writeRange.percentage = *v.percentage();
    writeSizes_.push_back(writeRange);
  }
  mergedSizes_.clear();
  auto mergedSizes = *benchmarkConfig_->mergedSizes();
  for (auto v : mergedSizes) {
    halcyon::IoSizeRange mergedRange;
    mergedRange.text = *v.text();
    mergedRange.lowerBound = *v.lowerBound();
    mergedRange.upperBound = *v.upperBound();
    mergedRange.percentage = *v.percentage();
    mergedSizes_.push_back(mergedRange);
  }
  mountpoints_.clear();
  mountpoints_ = *benchmarkConfig_->mountpoints();
  fileLayout_ = *benchmarkConfig_->fileLayout();
  threadsPerDisk_ = *benchmarkConfig_->runtimeConfig()->threadsPerDisk();
  networkThreads_ = *benchmarkConfig_->runtimeConfig()->networkThreads();
  qflushThreads_ = *benchmarkConfig_->runtimeConfig()->qflushThreads();
  pinThreads_ = *benchmarkConfig_->runtimeConfig()->pinThreads();
  qioCoresCount_ = *benchmarkConfig_->runtimeConfig()->qioCoresCount();
  qflushCoresCount_ = *benchmarkConfig_->runtimeConfig()->qflushCoresCount();
  pairPlaintext_ = *benchmarkConfig_->runtimeConfig()->pairPlaintext();
  ringEntries_ = *benchmarkConfig_->runtimeConfig()->ringEntries();
  queueDepth_ = *benchmarkConfig_->runtimeConfig()->queueDepth();
  busyPollDataPath_ = *benchmarkConfig_->runtimeConfig()->busyPollDataPath();
  fillIdleCores_ = *benchmarkConfig_->runtimeConfig()->fillIdleCores();
  reactorsPerMount_ = *benchmarkConfig_->runtimeConfig()->reactorsPerMount();
  qmetaThreads_ = *benchmarkConfig_->runtimeConfig()->qmetaThreads();
  cacheCapacity_ = *benchmarkConfig_->runtimeConfig()->cacheCapacity();
  cacheLocality_ = *benchmarkConfig_->runtimeConfig()->cacheLocality();
  cacheWindow_ = *benchmarkConfig_->runtimeConfig()->cacheWindow();
  runtime_ = *benchmarkConfig_->runtimeConfig()->runTime();
  warmup_ = *benchmarkConfig_->runtimeConfig()->warmupTime();
  qps_ = *benchmarkConfig_->runtimeConfig()->qps();
  doDirectIo_ = *benchmarkConfig_->runtimeConfig()->doDirectIo();
  doFileIo_ = *benchmarkConfig_->runtimeConfig()->doFileIo();
  doNetworkIo_ = *benchmarkConfig_->runtimeConfig()->doNetworkIo();
  doCacheIo_ = *benchmarkConfig_->runtimeConfig()->doCacheIo();
  backend_ =
      toEngineBackend(*benchmarkConfig_->runtimeConfig()->metadataBackend());
  partner_ = *benchmarkConfig_->nodeConfig()->partner();
  role_ = *benchmarkConfig_->nodeConfig()->role();
  // Server-side setup for PairRole::RequestResponse: open ChunkMappers +
  // fileset FDs so the chunkOp RPC handler can serve reads via pread. No-op
  // for other pair roles.
  try {
    initRequestResponseState();
  } catch (const std::exception& e) {
    LOG(ERROR) << "initRequestResponseState failed: " << e.what();
    auto resp = make_unique<HalcyonResponse>();
    *resp->return_code() = HalcyonReturnCode::DefaultError;
    *resp->message() = e.what();
    co_return resp;
  }
  LOG(INFO) << "Configuration Parameters";
  LOG(INFO) << sformat("Read Percentage: {:.2f}%", 100 * readPercentage_);
  LOG(INFO) << sformat("Threads Per Disk: {}", threadsPerDisk_);
  LOG(INFO) << sformat("Warmup (seconds): {}", warmup_);
  LOG(INFO) << sformat("Runtime (seconds): {}", runtime_);
  LOG(INFO) << sformat("Reporting Interval (seconds): {}", reportingInterval_);
  LOG(INFO) << sformat("QPS (0 = unthrottled): {}", qps_);
  if (!doDirectIo_) {
    LOG(INFO) << sformat("Direct I/O is disabled");
  } else {
    LOG(INFO) << sformat("Direct I/O is enabled");
  }
  auto resp = make_unique<HalcyonResponse>();
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->message() = "configuration set successfully";
  co_return resp;
}
Task<unique_ptr<HalcyonResponse>> HalcyonServiceHandler::co_startBenchmark() {
  auto resp = make_unique<HalcyonResponse>();
  // Reject a restart while a prior run is still in flight. The background
  // engine holds references to pools_/perfStats_; rebuilding them below would
  // destroy those objects out from under the still-running reactor threads (a
  // use-after-free). The caller must wait for isBenchmarkRunning() to clear.
  if (isBenchmarkRunning()) {
    LOG(ERROR) << "Benchmark already running; ignoring start request";
    *resp->return_code() = HalcyonReturnCode::DefaultError;
    *resp->message() = "Benchmark already running";
    co_return resp;
  }
  int n = mountpoints_.size();
  LOG(INFO) << "Clearing performance counters";
  diskMon_.clearCounters();
  cpuMon_.clearCounters();
  netMon_.clearCounters();
  // Drop any queue from a prior SendDiskReads / SendDiskBoth run so a
  // subsequent run (possibly with a different PairRole) starts clean.
  // Queues are only re-allocated below when the role requires them.
  sendQueue_.reset();
  recvQueue_.reset();
  // Stop and join --fill-idle-cores spin threads from any prior run before
  // rebuilding pools_ (whose accounting the threads reference).
  if (idleSpinStop_) {
    idleSpinStop_->store(true, std::memory_order_relaxed);
  }
  for (auto& t : idleSpinThreads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  idleSpinThreads_.clear();
  idleSpinStop_.reset();
  // Build the per-run storage pools (QIOThread + QFlush) when file I/O is
  // enabled; they are sized from this run's threadsPerDisk. The QNet pool is
  // NOT built here -- it is process-scoped (built once at server startup, sized
  // by
  // --qnet_pool_size) and injected as processPools_, so it can also back the
  // Thrift server. The network send workers (cooperative coroutines since
  // 4b-1) multiplex onto that fixed pool regardless of networkThreads_.
  if (doFileIo_) {
    halcyon::PoolConfig poolCfg;
    poolCfg.qioThreads = static_cast<size_t>(threadsPerDisk_) * n;
    // QFlush write-offload pool (rocksdb backend only; the engine guards on the
    // backend). 0 leaves chunk-map puts inline on the reactor thread.
    poolCfg.qflushThreads = static_cast<size_t>(qflushThreads_);
    // Core allocation. Reactor threads always get their own dedicated slice
    // so 1 reactor == 1 core; the qioPool background workers and qflush pool
    // never share pin slots with reactors.
    //
    // Auto layout (--qio-cores-count == 0, the default): read the machine's
    // core count at run-time and lay out
    //   [0, numReactors)                              -> reactor threads
    //   [numReactors, nCpus - qflushCount - kReserve) -> qioPool workers
    //   [nCpus - qflushCount, nCpus)                  -> qflush pool
    // The trailing kReserve cores are left free for QNet, Thrift IO,
    // kernel iou-wrk, and other unpinned system threads.
    //
    // Manual layout (--qio-cores-count > 0): honor the legacy range
    //   [0, qioCoresCount_) split into
    //     [0, numReactors)                            -> reactor threads
    //     [numReactors, qioCoresCount_)               -> qioPool workers
    //   [qioCoresCount_, qioCoresCount_+qflushCoresCount_) -> qflush pool
    // so existing scripts keep working.
    if (pinThreads_) {
      poolCfg.pinEnabled = true;
      const int numReactors = std::max(1, reactorsPerMount_) * n;
      constexpr int kReserve = 8;
      const int nCpus = std::max<int>(
          1, static_cast<int>(std::thread::hardware_concurrency()));
      int qflushCount = qflushCoresCount_;
      if (qflushCount <= 0 && qflushThreads_ > 0) {
        qflushCount = std::min(qflushThreads_, 8);
      }
      int reactorCoreCount = 0;
      int qioPoolStart = 0;
      int qioPoolEnd = 0;
      int qflushStart = 0;
      if (qioCoresCount_ > 0) {
        // Manual override.
        reactorCoreCount = std::min(numReactors, qioCoresCount_);
        qioPoolStart = reactorCoreCount;
        qioPoolEnd = qioCoresCount_;
        qflushStart = qioCoresCount_;
      } else {
        // Auto layout.
        const int maxReactorCores = std::max(1, nCpus - qflushCount - kReserve);
        reactorCoreCount = std::min(numReactors, maxReactorCores);
        qioPoolStart = reactorCoreCount;
        qioPoolEnd = std::max(qioPoolStart, nCpus - qflushCount - kReserve);
        qflushStart = nCpus - qflushCount;
      }
      for (int c = 0; c < reactorCoreCount; ++c) {
        poolCfg.reactorCores.push_back(c);
      }
      // When --fill-idle-cores is on, unpin the qioPool workers and hand
      // their cores to the idle-spin pool instead. The qioPool workers
      // still run (unpinned across all 144 cores), so background compaction
      // / GC still happens -- they just don't reserve specific pin slots
      // that would collide with the spin threads.
      if (!fillIdleCores_) {
        for (int c = qioPoolStart; c < qioPoolEnd; ++c) {
          poolCfg.qioCores.push_back(c);
        }
      }
      for (int c = qflushStart; c < qflushStart + qflushCount; ++c) {
        poolCfg.qflushCores.push_back(c);
      }
      if (fillIdleCores_) {
        // Fill everything between the reactor range and the qflush range --
        // BUT skip cores whose HT sibling is already handling real work
        // (reactor or qflush). A PAUSE-spinning thread on the sibling of a
        // busy thread evicts its L1/L2 lines and competes for the shared
        // execution ports on the physical core, which hurts IPC and
        // inflates memory bandwidth. Filtering keeps --fill-idle-cores
        // cosmetic: mpstat shows the box "full", but the reactor/qflush
        // threads still own their physical cores uncontested.
        std::unordered_set<int> busyCores(
            poolCfg.reactorCores.begin(), poolCfg.reactorCores.end());
        busyCores.insert(
            poolCfg.qflushCores.begin(), poolCfg.qflushCores.end());
        int htSkipped = 0;
        for (int c = reactorCoreCount; c < qflushStart; ++c) {
          if (busyCores.count(c)) {
            continue;
          }
          int sib = htSibling(c);
          if (sib >= 0 && busyCores.count(sib)) {
            ++htSkipped;
            continue;
          }
          poolCfg.idleSpinCores.push_back(c);
        }
        if (htSkipped > 0) {
          LOG(INFO) << sformat(
              "--fill-idle-cores: skipped {} cores whose HT sibling is a "
              "busy reactor/qflush thread (cache-safe filter)",
              htSkipped);
        }
      }
      if (reactorCoreCount < numReactors) {
        LOG(WARNING) << sformat(
            "Reactor demand ({}) exceeds available pin slots ({}); reactors "
            "will round-robin and oversubscribe cores",
            numReactors,
            reactorCoreCount);
      }
      LOG(INFO) << sformat(
          "Pinning enabled ({}): nCpus={} reactor_cores=[0,{}) "
          "qio_pool_cores=[{},{}) qflush_cores=[{},{}) idle_spin_cores={} ({})",
          qioCoresCount_ > 0 ? "manual" : "auto",
          nCpus,
          reactorCoreCount,
          fillIdleCores_ ? 0 : qioPoolStart,
          fillIdleCores_ ? 0 : qioPoolEnd,
          qflushStart,
          qflushStart + qflushCount,
          poolCfg.idleSpinCores.size(),
          fillIdleCores_ ? "enabled" : "off");
    }
    pools_ =
        std::make_unique<halcyon::HalcyonPools>(halcyon::buildPools(poolCfg));
    // --fill-idle-cores: spawn one pinned spin thread per idle core. Each
    // thread runs `while (!stop) pause()`. Threads share one atomic stop
    // flag held by shared_ptr so the flag outlives the threads.
    if (fillIdleCores_ && !poolCfg.idleSpinCores.empty()) {
      idleSpinStop_ = std::make_shared<std::atomic<bool>>(false);
      idleSpinThreads_.reserve(poolCfg.idleSpinCores.size());
      for (size_t i = 0; i < poolCfg.idleSpinCores.size(); ++i) {
        auto stop = idleSpinStop_;
        idleSpinThreads_.push_back(pools_->makeIdleSpinThread([stop] {
          while (!stop->load(std::memory_order_relaxed)) {
            for (int j = 0; j < 1024; ++j) {
              folly::asm_volatile_pause();
            }
          }
        }));
      }
      LOG(INFO) << sformat(
          "--fill-idle-cores: spawned {} pinned idle-spin threads",
          idleSpinThreads_.size());
    }
  }

  // Start file I/O
  if (doFileIo_) {
    LOG(INFO) << "Starting file I/O on " << n << " mount points";
    // PerfStats rows are indexed by OpSlot index in handleCompletion, so must
    // match the reactor's actual queueDepth. When --queue-depth decouples
    // from --threads-per-disk, we need the larger of the two to avoid an
    // out-of-bounds access on ists/csts. With --reactors-per-mount N, each
    // mount gets N reactors each with its own PerfStats row -- sized as
    // n * reactorsPerMount rows.
    const int perfSlots = std::max(threadsPerDisk_, queueDepth_);
    const int perfRows = n * std::max(1, reactorsPerMount_);
    unique_ptr<halcyon::PerfStats> perfStats(
        new halcyon::PerfStats(perfRows, perfSlots));
    perfStats_ = std::move(perfStats);

    // The ring must hold every in-flight op. --queue-depth is decoupled from
    // --threads-per-disk, so the in-flight depth is the larger of the two;
    // size the ring to it (over the 256 default / runtime override). Runtime
    // override raises the default so deeper rings can queue more concurrent
    // ops -- matters when --queue-depth keeps more ops outstanding.
    unsigned ringEntries =
        ringEntries_ > 0 ? static_cast<unsigned>(ringEntries_) : 256u;
    if (static_cast<unsigned>(threadsPerDisk_) > ringEntries) {
      ringEntries = static_cast<unsigned>(threadsPerDisk_);
    }
    if (queueDepth_ > 0 && static_cast<unsigned>(queueDepth_) > ringEntries) {
      ringEntries = static_cast<unsigned>(queueDepth_);
    }
    halcyon::IoUringEngine::Config engineConfig;
    engineConfig.mountPoints = std::vector<std::string>(
        mountpoints_.begin(), mountpoints_.begin() + n);
    engineConfig.warmupSec = warmup_;
    engineConfig.runtimeSec = runtime_;
    engineConfig.ringEntries = ringEntries;
    engineConfig.threadsPerDisk = threadsPerDisk_;
    engineConfig.doDirectIo = doDirectIo_;
    engineConfig.readRatio = readPercentage_;
    engineConfig.iops = (n > 0) ? qps_ / n : 0.0;
    engineConfig.readSizes = readSizes_;
    engineConfig.writeSizes = writeSizes_;
    engineConfig.backend = backend_;
    // Match the QFlush worker count to the pool buildPools created above, so
    // the reactors offload chunk-map writes (rocksdb backend only; no-op for
    // 0).
    engineConfig.qflushThreads = qflushThreads_;
    // Per-reactor read cache + injected read locality (rocksdb backend only;
    // the engine guards on the backend).
    engineConfig.cacheCapacity = static_cast<size_t>(cacheCapacity_);
    engineConfig.readLocality = cacheLocality_;
    engineConfig.localityWindow = static_cast<halcyon::ChunkId>(cacheWindow_);
    engineConfig.perfStats = perfStats_.get();
    engineConfig.queueDepth = queueDepth_;
    engineConfig.busyPollDataPath = busyPollDataPath_;
    engineConfig.reactorsPerMount = reactorsPerMount_;
    engineConfig.qmetaThreads = qmetaThreads_;

    // PairRole::SendDiskReads: allocate the shared reactor->network queue
    // that carries actual read bytes to the QNet workers. Sized generously
    // relative to concurrent in-flight reads across all reactors so brief
    // NIC hiccups don't stall the reactor, but sustained NIC saturation
    // will fill it and back-pressure disk submission -- the whole point of
    // the coupling. Every other PairRole leaves sendQueue_ null.
    if (role_ == PairRole::SendDiskReads || role_ == PairRole::SendDiskBoth) {
      const size_t sendQueueCapacity =
          std::max<size_t>(static_cast<size_t>(threadsPerDisk_) * n * 4, 512);
      sendQueue_ = std::make_unique<halcyon::SendDataQueue>(sendQueueCapacity);
      engineConfig.sendQueue = sendQueue_.get();
      // Shared aligned-buffer pool so reactors hand a completed read's buffer
      // to the network layer by moving ownership instead of memcpy (removes the
      // per-read copy that burns memory bandwidth). Sized to cover every
      // reactor's in-flight ops (totalSlots) plus a full sendQueue of
      // handed-off buffers, so pool exhaustion never precedes the sendQueue's
      // bounded blockingWrite as the backpressure point. Each buffer is
      // kMaxFileSize and resident for the whole run, so this pool adds
      // ~(totalSlots + sendQueueCapacity) * kMaxFileSize of RSS versus the old
      // copy path's transient per-read heap buffers.
      const size_t effectiveQueueDepth =
          static_cast<size_t>(queueDepth_ > 0 ? queueDepth_ : threadsPerDisk_);
      const size_t totalSlots = effectiveQueueDepth * static_cast<size_t>(n) *
          static_cast<size_t>(std::max(1, reactorsPerMount_));
      const size_t poolBuffers = totalSlots + sendQueueCapacity;
      bufferPool_ = std::make_unique<halcyon::IoBufferPool>(
          poolBuffers, static_cast<size_t>(halcyon::kMaxFileSize));
      engineConfig.bufferPool = bufferPool_.get();
      LOG(INFO) << sformat(
          "PairRole::{}: sendQueue capacity={} entries, bufferPool={} buffers "
          "x {} bytes",
          role_ == PairRole::SendDiskBoth ? "SendDiskBoth" : "SendDiskReads",
          sendQueueCapacity,
          poolBuffers,
          static_cast<size_t>(halcyon::kMaxFileSize));
    }

    // PairRole::SendDiskBoth: allocate the shared server->reactor receive
    // queue that carries incoming write payloads from co_sendData to the
    // reactors. Same capacity heuristic as sendQueue -- big enough to
    // smooth over brief network jitter, small enough that sustained
    // receiver-disk saturation back-pressures the sender via Thrift RPC.
    if (role_ == PairRole::SendDiskBoth) {
      const size_t recvQueueCapacity =
          std::max<size_t>(static_cast<size_t>(threadsPerDisk_) * n * 4, 512);
      recvQueue_ = std::make_unique<halcyon::RecvDataQueue>(recvQueueCapacity);
      engineConfig.recvQueue = recvQueue_.get();
      LOG(INFO) << sformat(
          "PairRole::SendDiskBoth: recvQueue capacity={} entries",
          recvQueueCapacity);
    }

    // run() blocks (it joins its reactor threads), so drive it on a background
    // task: the RPC returns immediately and the client polls isBenchmarkRunning
    // / co_getPerfStats while the engine runs.
    fileIoFuture_ = async(
        launch::async,
        [this, engineConfig = std::move(engineConfig)]() mutable {
          halcyon::IoUringEngine engine(*pools_, std::move(engineConfig));
          engine.run();
        });
  } else {
    LOG(INFO) << "File I/O is disabled.";
  }
  // Start network I/O
  if (doNetworkIo_) {
    LOG(INFO) << "Starting network I/O";
    double netRate = qps_ / networkThreads_;
    vector<halcyon::IoSizeRange> ioSizes;
    switch (role_) {
      case (PairRole::SendAll):
        ioSizes = mergedSizes_;
        break;
      case (PairRole::SendHalf):
        ioSizes = mergedSizes_;
        netRate /= 2;
        break;
      case (PairRole::SendReads):
        ioSizes = readSizes_;
        break;
      case (PairRole::SendWrites):
        ioSizes = writeSizes_;
        break;
      case (PairRole::SendNone):
        netRate = 0;
        break;
      case (PairRole::SendDiskReads):
        // Byte sizes are determined per-op by the reactor's actual read
        // sizes (each dequeued SendItem carries its size); ioSizes is left
        // empty and unused by the disk-derived path in NetworkOperator.
        // netRate is unthrottled -- the queue itself paces the sender to
        // whatever rate the reactor produces (which itself is bounded by
        // NIC drain when the queue fills up).
        break;
      case (PairRole::SendDiskBoth):
        // Same as SendDiskReads for the network sender: each SendItem
        // (read-derived or write-payload) carries its own size. The
        // sendQueue paces the sender; on the receiver side, co_sendData
        // routes items tagged Op::Write into recvQueue for real disk
        // writes. ioSizes stays empty for the same reason.
        break;
      case (PairRole::RequestResponse):
        // PairRole::RequestResponse: sender-side workers issue chunkOp RPCs
        // at the partner. Each op picks a random size from mergedSizes and
        // routes Read vs Write by readPercentage_. Reads receive real chunk
        // bytes from the partner's fileset; writes send a payload the
        // partner ACKs. Concurrency = networkThreads_.
        ioSizes = mergedSizes_;
        break;
    }
    unique_ptr<halcyon::NetStats> netStats(
        new halcyon::NetStats(networkThreads_));
    for (int i = 0; i < networkThreads_; i++) {
      // Each send worker runs on the process-scoped QNet pool, so its CPU is
      // named, pinnable, and accounted. Since 4b-1 the worker is a cooperative
      // coroutine, so networkThreads_ workers multiplex onto the fixed
      // --qnet_pool_size pool. launchNetworkOperator submits the worker and
      // returns.
      halcyon::launchNetworkOperator(
          i,
          runtime_,
          warmup_,
          netRate,
          partner_,
          ioSizes,
          netStats.get(),
          &processPools_->pool(halcyon::PoolRole::QNet),
          // Non-null only for PairRole::SendDiskReads; the worker switches
          // to the disk-derived path (dequeue real read buffers instead of
          // memcpy from RAM).
          sendQueue_.get(),
          // Dev-only plaintext: skip SR/TLS for partner RPCs. Matches
          // fb-FIO Hypernode's tls=0. Required on rtptest hosts without
          // valid halcyon.test SR identity.
          pairPlaintext_,
          // Hypernode-style busy-poll: swap `co_await sleep` for
          // asm_volatile_pause on empty sendQueue. Pins the QNet thread
          // at ~100% CPU per worker, matching hn.dptworker.
          busyPollDataPath_,
          // PairRole::RequestResponse: worker fires chunkOp RPCs instead
          // of sendData. readPercentage_ selects Read vs Write per op;
          // chunkIdRange bounds the random-chunkId picker to what the
          // partner's chunkmap.rocksdb actually covers (100k is safe --
          // we validated up to 1M in the smoke test).
          role_ == PairRole::RequestResponse,
          readPercentage_,
          /*chunkIdRange=*/100000);
    }
    netStats_ = std::move(netStats);
  } else {
    LOG(INFO) << "Network I/O is disabled";
  }
  LOG(INFO) << "Successfully started benchmark";
  *resp->return_code() = HalcyonReturnCode::Success;
  *resp->message() = "Successfully started benchmark";
  co_return resp;
}

bool HalcyonServiceHandler::isBenchmarkRunning() {
  bool fileIoActive = false;
  bool networkIoActive = false;
  bool cacheIoActive = false;
  if (doFileIo_) {
    fileIoActive = fileIoFuture_.valid() &&
        fileIoFuture_.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready;
  }
  // netStats_ is populated inside startBenchmark; on the very first call the
  // restart guard in co_startBenchmark reaches isBenchmarkRunning() before
  // that assignment, so netStats_ can be null. Treat null as "no run in
  // flight" rather than dereferencing.
  if (doNetworkIo_ && netStats_) {
    for (auto i : netStats_->activeIo) {
      if (i) {
        networkIoActive = true;
        break;
      }
    }
  }
  if (doFileIo_ && !fileIoActive) {
    LOG(INFO) << "File I/O is not active";
  }
  if (doNetworkIo_ && !networkIoActive) {
    LOG(INFO) << "Network I/O is not active";
  }
  if (fileIoActive || networkIoActive || cacheIoActive) {
    LOG(INFO) << "Benchmark is running";
    return true;
  }
  LOG(INFO) << "Benchmark is not running";
  return false;
};

void HalcyonServiceHandler::stopBenchmark() {
  pleaseStop = true;
}

// Performance monitoring methods
int HalcyonServiceHandler::getLinkSpeed() {
  return netMon_.getLinkSpeed();
}
double HalcyonServiceHandler::getDiskUtilization() {
  return diskMon_.getUtilization();
}
double HalcyonServiceHandler::getCpuUtilization() {
  return cpuMon_.getUtilization();
}
double HalcyonServiceHandler::getNetworkThroughput() {
  return netMon_.getThroughput();
}
double HalcyonServiceHandler::getDiskUtilizationFromBeginning() {
  return diskMon_.getUtilizationFromBeginning();
}
double HalcyonServiceHandler::getCpuUtilizationFromBeginning() {
  return cpuMon_.getUtilizationFromBeginning();
}
double HalcyonServiceHandler::getNetworkThroughputFromBeginning() {
  return netMon_.getThroughputFromBeginning();
}

Task<unique_ptr<PerfStats>> HalcyonServiceHandler::co_getPerfStats() {
  auto resp = make_unique<PerfStats>();
  if (!doFileIo_) {
    co_return resp;
  }
  *resp->numOperators() = perfStats_->nOperators;
  *resp->numWorkers() = perfStats_->nWorkers;
  resp->istats()->resize(perfStats_->istats.size());
  for (int i = 0; i < perfStats_->istats.size(); i++) {
    int s = perfStats_->istats.at(i).size();
    resp->istats()[i].resize(s);
    for (int j = 0; j < s; j++) {
      *resp->istats()->at(i).at(j).readIos() =
          perfStats_->istats.at(i).at(j).readIos;
      *resp->istats()->at(i).at(j).writeIos() =
          perfStats_->istats.at(i).at(j).writeIos;
      *resp->istats()->at(i).at(j).readBytes() =
          perfStats_->istats.at(i).at(j).readBytes;
      *resp->istats()->at(i).at(j).writeBytes() =
          perfStats_->istats.at(i).at(j).writeBytes;
      *resp->istats()->at(i).at(j).readUsec() =
          perfStats_->istats.at(i).at(j).readUsec;
      *resp->istats()->at(i).at(j).writeUsec() =
          perfStats_->istats.at(i).at(j).writeUsec;
      *resp->istats()->at(i).at(j).usefulBusyNs() =
          perfStats_->istats.at(i).at(j).usefulBusyNs;
      *resp->istats()->at(i).at(j).usefulIdleNs() =
          perfStats_->istats.at(i).at(j).usefulIdleNs;
    }
  }
  resp->cstats()->resize(perfStats_->cstats.size());
  for (int i = 0; i < perfStats_->cstats.size(); i++) {
    int s = perfStats_->cstats.at(i).size();
    resp->cstats()[i].resize(s);
    for (int j = 0; j < perfStats_->cstats.at(i).size(); j++) {
      *resp->cstats()->at(i).at(j).readCount() =
          perfStats_->cstats.at(i).at(j).readCount;
      *resp->cstats()->at(i).at(j).writeCount() =
          perfStats_->cstats.at(i).at(j).writeCount;
      *resp->cstats()->at(i).at(j).readNanos() =
          perfStats_->cstats.at(i).at(j).readNanos;
      *resp->cstats()->at(i).at(j).writeNanos() =
          perfStats_->cstats.at(i).at(j).writeNanos;
    }
  }
  co_return resp;
};

Task<unique_ptr<NetworkStats>> HalcyonServiceHandler::co_getNetworkStats() {
  auto resp = make_unique<NetworkStats>();
  // Same rationale as isBenchmarkRunning: the client's reporting loop can poll
  // getNetworkStats before startBenchmark has populated netStats_. Return an
  // empty response instead of dereferencing null.
  if (!doNetworkIo_ || !netStats_) {
    co_return resp;
  }
  uint64_t sendIos = 0;
  uint64_t sendBytes = 0;
  uint64_t sendUsec = 0;
  for (int i = 0; i < netStats_->nstats.size(); i++) {
    sendIos += netStats_->nstats.at(i).sendIos;
    sendBytes += netStats_->nstats.at(i).sendBytes;
    sendUsec += netStats_->nstats.at(i).sendUsec;
  }
  resp->numOperators() = networkThreads_;
  resp->sendIos() = sendIos;
  resp->sendBytes() = sendBytes;
  resp->sendUsec() = sendUsec;
  // *resp->recvBytes_ref() = bytes_;
  // *resp->recvIos_ref() = transactions_;
  co_return resp;
};

Task<unique_ptr<CpuStats>> HalcyonServiceHandler::co_getCpuStats() {
  auto resp = make_unique<CpuStats>();
  halcyon::CpuStatCounters cpuCounts = cpuMon_.getCounts();
  resp->ts() = cpuCounts.ts;
  resp->user() = cpuCounts.user;
  resp->nice() = cpuCounts.nice;
  resp->system() = cpuCounts.system;
  resp->idle() = cpuCounts.idle;
  resp->iowait() = cpuCounts.iowait;
  resp->nCpu() = cpuCounts.nCpu;
  resp->ticksPerSec() = cpuMon_.ticksPerSec;
  co_return resp;
};

HalcyonServiceHandler::~HalcyonServiceHandler() {
  // Stop and join any --fill-idle-cores spin threads so their std::thread
  // dtors don't terminate() with joinable threads.
  if (idleSpinStop_) {
    idleSpinStop_->store(true, std::memory_order_relaxed);
  }
  for (auto& t : idleSpinThreads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  idleSpinThreads_.clear();
  idleSpinStop_.reset();
  free(benchmarkConfig_);
}

} // namespace facebook
