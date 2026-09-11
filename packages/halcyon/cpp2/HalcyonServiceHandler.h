// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "CpuMonitor.h"
#include "DiskMonitor.h"
#include "NetworkMonitor.h"

#include <folly/coro/Task.h>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "CreateStats.h"
#include "HalcyonChunkMapper.h"
#include "IoUringEngine.h" // SendDataQueue
#include "NetworkOperator.h"
#include "PerfStats.h"
#include "ThreadPools.h"
#include "if/gen-cpp2/HalcyonService.h"

namespace facebook {

class HalcyonServiceHandler : public cea::halcyon::HalcyonServiceSvIf {
 public:
  /// `processPools` is the process-scoped pool set (built once at server
  /// startup); the QNet pool from it runs the network send workers and backs
  /// the Thrift server. Per-run storage pools are still built per benchmark.
  explicit HalcyonServiceHandler(
      std::shared_ptr<halcyon::HalcyonPools> processPools);
  ~HalcyonServiceHandler() override;

  bool pleaseStop;

  // Partner node methods
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_setPartner(std::unique_ptr<std::string> hostname) override;
  folly::coro::Task<std::unique_ptr<std::string>> co_getPartner() override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_setPairRole(cea::halcyon::PairRole role) override;
  folly::coro::Task<cea::halcyon::PairRole> co_getPairRole() override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>> co_sendData(
      std::unique_ptr<cea::halcyon::HalcyonRequest> request) override;
  // PairRole::RequestResponse RPC handler. See halcyon.thrift for semantics:
  // Read requests reply with chunk bytes; Write requests pwrite and return an
  // empty payload.
  folly::coro::Task<std::unique_ptr<cea::halcyon::ChunkOpResponse>> co_chunkOp(
      std::unique_ptr<cea::halcyon::ChunkOpRequest> req) override;

 private:
  // Opens one HalcyonChunkMapper and the fileset FDs for every mount in
  // mountpoints_, keyed by mount index. Called by setBenchmarkConfiguration
  // when role_ == RequestResponse; a no-op otherwise. Idempotent -- closes
  // any previously opened FDs first.
  void initRequestResponseState();

 public:
  // Fileset creation methods
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_setFilesetLayout(
      std::unique_ptr<cea::halcyon::FileLayout> layout) override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_startFilesetCreation() override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::CreateStats>>
  co_getFilesetCreationStats() override;
  bool isFilesetCreationRunning() override;
  double getFilesetCreationProgress() override;
  void stopFilesetCreation() override;

  // Benchmark execution methods
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_setBenchmarkConfiguration(
      std::unique_ptr<cea::halcyon::BenchmarkConfiguration> config) override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::HalcyonResponse>>
  co_startBenchmark() override;
  bool isBenchmarkRunning() override;
  void stopBenchmark() override;

  // Performance monitoring methods
  int getLinkSpeed() override;
  double getDiskUtilization() override;
  double getCpuUtilization() override;
  double getNetworkThroughput() override;
  double getDiskUtilizationFromBeginning() override;
  double getCpuUtilizationFromBeginning() override;
  double getNetworkThroughputFromBeginning() override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::PerfStats>> co_getPerfStats()
      override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::NetworkStats>>
  co_getNetworkStats() override;
  folly::coro::Task<std::unique_ptr<cea::halcyon::CpuStats>> co_getCpuStats()
      override;

 private:
  std::string partner_;
  uint64_t transactions_;
  uint64_t bytes_;
  double readPercentage_;
  int threadsPerDisk_;
  int networkThreads_;
  int qflushThreads_;
  bool pinThreads_ = false;
  int qioCoresCount_ = 0;
  int qflushCoresCount_ = 0;
  // Dev-only: NetworkOperator opens a direct plaintext channel to the
  // partner instead of getSRClientUnique + TLS. See RuntimeConfiguration
  // in halcyon.thrift for rationale.
  bool pairPlaintext_ = false;
  // io_uring ring entries per reactor. 0 == legacy default (256, or
  // threadsPerDisk if larger). See RuntimeConfiguration.ringEntries.
  int ringEntries_ = 0;
  // Per-reactor OpSlot count (max in-flight ops per reactor). 0 == legacy
  // (opSlots == threadsPerDisk). See RuntimeConfiguration.queueDepth.
  int queueDepth_ = 0;
  // Hypernode-style busy-poll on all data-path pools. Threaded to
  // NetworkOperator (co_await sleep -> pause) and to IoUringEngine::Config
  // for FlushWorker + MetadataWorker (50us sleep -> pause). See
  // RuntimeConfiguration.busyPollDataPath.
  bool busyPollDataPath_ = false;
  // Opt-in: spawn one pinned idle-spin thread per otherwise-unused core so
  // halcyon's mpstat shape matches Hypernode's "one perpetually spinning
  // thread per physical core" model. See RuntimeConfiguration.fillIdleCores.
  bool fillIdleCores_ = false;
  // Multi-reactor per mount (experimental, manifest-only). Threaded to
  // IoUringEngine::Config -- see RuntimeConfiguration.reactorsPerMount.
  int reactorsPerMount_ = 1;
  // Number of MetadataWorker instances for chunk-map READ offload (RocksDb
  // backend only). 0 keeps inline lookups. See
  // RuntimeConfiguration.qmetaThreads.
  int qmetaThreads_ = 0;
  int cacheCapacity_;
  double cacheLocality_;
  int cacheWindow_;
  int runtime_;
  int warmup_;
  int reportingInterval_;
  int linkSpeed_;
  double qps_;
  bool doDirectIo_;
  bool doFileIo_;
  bool doNetworkIo_;
  bool doCacheIo_;
  halcyon::MetadataBackend backend_{halcyon::MetadataBackend::Manifest};
  cea::halcyon::BenchmarkConfiguration* benchmarkConfig_;
  // Shared ownership so async fileset-populate workers hold the object alive
  // for their lifetime even if a subsequent startFilesetCreation swaps it out,
  // and so getFilesetCreationStats can snapshot a strong ref locally to defend
  // against the Thrift coroutine initial-resume UAF race when the shared CPU
  // executor is starved by data-plane compute.
  std::shared_ptr<halcyon::CreateStats> createStats_;
  mutable std::mutex createStatsMu_;
  // Returns a strong reference to the current stats object. Readers hold the
  // returned shared_ptr for the duration of their traversal, so a concurrent
  // startFilesetCreation swap cannot free it underneath them.
  std::shared_ptr<halcyon::CreateStats> snapshotCreateStats() const;
  std::unique_ptr<halcyon::PerfStats> perfStats_;
  std::unique_ptr<halcyon::NetStats> netStats_;
  // Process-scoped pools (injected at construction): owns the QNet pool that
  // runs the network send workers and (4b-3) backs the Thrift server. Outlives
  // every benchmark run.
  std::shared_ptr<halcyon::HalcyonPools> processPools_;
  // Per-run storage pools (QIOThread + QFlush), rebuilt each benchmark from the
  // run's threadsPerDisk; null until the first file-I/O run.
  std::unique_ptr<halcyon::HalcyonPools> pools_;
  // PairRole::SendDiskReads only: bounded MPMC queue that carries each
  // completed read's bytes from IoUringEngine reactors to the QNet
  // NetworkOperator workers. When NIC saturates, blockingWrite on the
  // producer side back-pressures the reactor, capping file_xput at whatever
  // the network can drain. Null for every other PairRole.
  std::unique_ptr<halcyon::SendDataQueue> sendQueue_;
  // PairRole::SendDiskBoth only: bounded MPMC queue that carries incoming
  // write payloads from co_sendData (Thrift server) to IoUringEngine
  // reactors, which submit them as real io_uring pwrites instead of
  // generating write data locally from RAM. When the reactor can't keep
  // up, blockingWrite in co_sendData naturally back-pressures the sender
  // via Thrift RPC. Null for every other PairRole.
  std::unique_ptr<halcyon::RecvDataQueue> recvQueue_;
  // Disk-derived paired modes only: shared pool of aligned I/O buffers that
  // lets reactors hand a completed read's buffer to the network layer by
  // moving ownership instead of copying (removes the per-read memcpy, which
  // burns memory bandwidth and has no analog in the HyperNode read path this
  // models). Owned here so it outlives both the engine future and the QNet
  // NetworkOperator workers, which return buffers via the SendItem deleter.
  // Declared before fileIoFuture_ so it is destroyed after the engine future.
  // Null for every other PairRole.
  std::unique_ptr<halcyon::IoBufferPool> bufferPool_;
  // --fill-idle-cores state: N pinned std::threads (one per core in
  // idleSpinCores), each running `while (!*idleSpinStop_) pause()`. Owned
  // per benchmark run; joined at teardown so the atomic outlives them.
  std::shared_ptr<std::atomic<bool>> idleSpinStop_;
  std::vector<std::thread> idleSpinThreads_;
  // PairRole::RequestResponse server-side state. When a benchmark with
  // RequestResponse role is configured, we pre-open one HalcyonChunkMapper
  // and the fileset FDs per mount, so co_chunkOp can do synchronous
  // pread/pwrite off the QNet coroutine thread without touching the reactor
  // engine. Cleared before every benchmark run.
  std::vector<std::shared_ptr<halcyon::HalcyonChunkMapper>> rrMappers_;
  // rrFds_[mount_idx] -> list of fds for that mount's fileset.
  std::vector<std::vector<int>> rrFds_;
  // HN workload emulation state (see --rr_hn_workload). Simulates HN's per-op
  // stats bookkeeping: one atomic counter per op, one bytes accumulator, and
  // a 32-bucket "latency histogram" that spreads writes across cache lines
  // to reproduce HN's per-op state cache-miss density. Deliberately contended
  // across cores so the atomic RMWs cause real cache-line ping-pong, matching
  // HN's per-connection state contention.
  static constexpr size_t kHnStatsBuckets = 32;
  std::atomic<uint64_t> hnStatsCounter_{0};
  std::atomic<uint64_t> hnStatsBytes_{0};
  std::array<std::atomic<uint64_t>, kHnStatsBuckets> hnStatsLatencyBucket_{};
  std::vector<std::future<int>> cfuts_;
  std::vector<std::string> mountpoints_;
  std::vector<halcyon::IoSizeRange> readSizes_;
  std::vector<halcyon::IoSizeRange> writeSizes_;
  std::vector<halcyon::IoSizeRange> mergedSizes_;
  cea::halcyon::PairRole role_;
  cea::halcyon::FileLayout fileLayout_;
  halcyon::DiskMonitor diskMon_;
  halcyon::CpuMonitor cpuMon_;
  halcyon::NetworkMonitor netMon_;
  // Background task running the io_uring engine for the file-I/O benchmark.
  // Declared last so it is destroyed first: its destructor waits for the engine
  // to finish before pools_/perfStats_ (which it references) are torn down.
  std::future<void> fileIoFuture_;
};

} // namespace facebook
