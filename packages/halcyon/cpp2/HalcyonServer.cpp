// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <memory>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/async/IoUringBackend.h>
#include <folly/io/async/IoUringOptions.h>
#include <thrift/lib/cpp2/Flags.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "ThreadPools.h"
#include "cpp2/HalcyonServiceHandler.h"

THRIFT_FLAG_DECLARE_string(rocket_frame_parser);

DEFINE_int32(port, 23459, "Port for halcyon server");
DEFINE_int32(
    qnet_pool_size,
    16,
    "Process-scoped QNet pool size: threads that run the network send workers "
    "and serve inbound RPC. Fixed for the server lifetime; the network send "
    "workers are cooperative coroutines that multiplex onto it.");
DEFINE_int32(
    qmeta_pool_size,
    1,
    "Process-scoped QMetadata pool size: OS threads available for chunk-map "
    "read-lookup workers (MetadataWorker). Client-side --qmeta-threads N "
    "spawns N MetadataWorker instances on this pool; effective concurrency "
    "= min(pool_size, worker_count). Default 1 keeps legacy inline-lookup "
    "behavior when --qmeta-threads is 0. Raise to match --qmeta-threads for "
    "Hypernode-style chunkMapper thread accounting.");
DEFINE_int32(
    io_threads,
    0,
    "Thrift IO worker threads (accept + event loops). 0 = Thrift default "
    "(hardware concurrency).");
DEFINE_int32(
    io_uring_provided_bufs,
    4096,
    "Fix C: io_uring provided-buffer count for RX. Default 4096 buffers of "
    "2 KB each = 8 MB per Thrift IO worker. Reducing (512, 1024) shrinks the "
    "per-worker recv working set to fit L2, dropping halcyon's cache footprint "
    "closer to HN's Dbuf pool efficiency on SRF/SKL.");
DEFINE_int32(
    thrift_queue_timeout_ms,
    60000,
    "Server-side Thrift request-queue timeout in ms. Default 60s (up from "
    "Thrift's default 100ms) so heavy per-op compute in the shared executor "
    "cannot shed short monitoring RPCs (getDiskUtilization etc.) when the "
    "queue deepens. Set 0 to leave Thrift default in place.");
DEFINE_int32(
    thrift_task_expire_ms,
    0,
    "Server-side Thrift task-expire (execution deadline) in ms. 0 disables "
    "server-side task expiration; useful when running with heavy per-op "
    "compute where individual chunkOp handlers exceed the default 30s.");
DECLARE_bool(use_io_uring_recv);

using namespace facebook;

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  // Force Rocket's AlignedParserStrategy on the server (and on any Rocket
  // client this process constructs, e.g. NetworkOperator's plaintext
  // channel). AlignedParserStrategy allocates a dedicated contiguous IOBuf
  // per PAYLOAD frame -- unchained, jemalloc-aligned -- which is what the
  // recv-write zero-copy fast path in IoUringEngine::submitRecvWrite
  // requires (unchained + word-aligned + LBA-multiple length). The default
  // parser ("strategy") produces small-head + big-tail chains via the
  // shared IOBufQueue, and the predicate would fail 100% of the time.
  // Matches Hypernode's rocket_frame_parser="aligned" setting that makes
  // its hypernode.aligned_parser_put counter fire in production.
  THRIFT_FLAG_SET_MOCK(rocket_frame_parser, "aligned");
  // Build the process-scoped pools once, before serving: the QNet pool must
  // outlive every benchmark run and back the Thrift server, so it cannot be
  // sized per-run from RPC config the way the storage pools are.
  halcyon::PoolConfig poolCfg;
  poolCfg.qnetThreads = static_cast<size_t>(FLAGS_qnet_pool_size);
  poolCfg.qmetaThreads = static_cast<size_t>(FLAGS_qmeta_pool_size);
  auto processPools =
      std::make_shared<halcyon::HalcyonPools>(halcyon::buildPools(poolCfg));
  auto handler = std::make_shared<HalcyonServiceHandler>(processPools);

  // Explicit ThriftServer so inbound RPC handlers run on the *same* QNet pool
  // as the outbound network send workers -- the single combined network/RPC
  // pool of plan #4b. setThreadManagerFromExecutor backs the default-async
  // (CPU) resource pool with the QNet executor, so RPC-handler CPU is accounted
  // under getCpuNs(QNet). Sharing the pool with the send workers is safe
  // because they are cooperative coroutines (4b-1) that yield the thread
  // between sends. processPools outlives `server` (declared first, destroyed
  // last), so the executor stays alive for the server's lifetime.
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  server->setInterface(handler);
  server->setPort(FLAGS_port);
  if (FLAGS_io_threads > 0) {
    server->setNumIOWorkerThreads(static_cast<size_t>(FLAGS_io_threads));
  }
  server->setThreadManagerFromExecutor(
      &processPools->pool(halcyon::PoolRole::QNet), "QNet");
  // Raise queue timeout so heavy compute per op can't starve short monitoring
  // RPCs. Default 60s (Thrift default is 100ms which sheds every stat RPC when
  // client-side runs c=1M+ compute knobs).
  if (FLAGS_thrift_queue_timeout_ms > 0) {
    server->setQueueTimeout(
        std::chrono::milliseconds(FLAGS_thrift_queue_timeout_ms));
    LOG(INFO) << "queueTimeout = " << FLAGS_thrift_queue_timeout_ms << " ms";
  }
  if (FLAGS_thrift_task_expire_ms >= 0) {
    server->setTaskExpireTime(
        std::chrono::milliseconds(FLAGS_thrift_task_expire_ms));
    LOG(INFO) << "taskExpireTime = " << FLAGS_thrift_task_expire_ms << " ms";
  }
  // Ignore client-side deadlines. When a heavy chunkOp is in flight and the
  // Python client's default 100ms timeout on getDiskUtilization fires, we don't
  // want the server to cancel the in-flight stats RPC and race with the
  // coroutine frame.
  server->setUseClientTimeout(false);

  // Fix #3: install an IOThreadPool whose EventBases run IoUringBackend with
  // nativeAsyncSocketSupport=true, then flip preferIoUring on the server.
  // Cpp2Worker checks AsyncIoUringSocketFactory::supports(evb) on every
  // plaintext accept and, if true, upgrades the accepted socket to
  // AsyncIoUringSocket -- which uses IORING_OP_RECV_MULTISHOT into provided
  // buffers, bypassing tcp_recvmsg's skb->user memcpy. supports() returns
  // false when the backend fails to init (older kernel etc.), so this is
  // safe to leave on by default.
  std::shared_ptr<folly::IOThreadPoolExecutor> ioPool;
  std::shared_ptr<folly::EventBaseManager> ioUringEbm;
  if (FLAGS_use_io_uring_recv) {
    const size_t n = FLAGS_io_threads > 0
        ? static_cast<size_t>(FLAGS_io_threads)
        : std::max<size_t>(1, std::thread::hardware_concurrency());
    folly::EventBase::Options ebOpts;
    ebOpts.setBackendFactory(
        []() -> std::unique_ptr<folly::EventBaseBackendBase> {
          folly::IoUringOptions opts;
          opts.setInitialProvidedBuffers(FLAGS_io_uring_provided_bufs, 2000)
              .setMaxSubmit(256)
              .setCapacity(4096)
              .setRegisterRingFd(true)
              .setDeferTaskRun(true);
          return std::make_unique<folly::IoUringBackend>(std::move(opts));
        });
    // Custom EventBaseManager whose per-thread EventBase is constructed with
    // our IoUringBackend factory. IOThreadPoolExecutor calls getEventBase()
    // on this manager to obtain each worker's EventBase.
    ioUringEbm = std::make_shared<folly::EventBaseManager>(std::move(ebOpts));
    ioPool = std::make_shared<folly::IOThreadPoolExecutor>(
        n,
        std::make_shared<folly::NamedThreadFactory>("HalcyonIoUring"),
        ioUringEbm.get());
    server->setIOThreadPool(ioPool);
    server->setNumIOWorkerThreads(n);
    // setPreferIoUring() removed from ThriftServer API in newer fbthrift;
    // IoUringBackend is now selected via the ioPool's EventBaseManager.
    LOG(INFO) << "Fix #3: IoUringBackend IOThreadPool installed (" << n
              << " workers)";
  }

  // Plain ThriftServer::serve() -- no ServiceFramework/fb303 wrapper, to keep
  // halcyon open-source-friendly. Blocks until the server is stopped.
  server->serve();
  return 0;
}
