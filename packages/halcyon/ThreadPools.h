// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <folly/Function.h>
#include <folly/Range.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/thread_factory/InitThreadFactory.h>

namespace facebook::halcyon {

/// Logical role of a Halcyon thread pool. Each role maps to a single named,
/// optionally CPU-pinned executor so its CPU cost can be measured in isolation.
enum class PoolRole {
  Reactor, // io_uring IO-Core reactor std::threads (per-mount, dedicated core)
  QIOThread, // background disk-side workers on the QIO pool (compaction/GC)
  QMetadata, // RocksDB chunk-map lookup workers (async, off the reactor thread)
  QNet, // synthetic network send workers (NetworkOperator -> partner
        // co_sendData)
  QFlush, // RocksDB chunk-map write workers (batched putBatch + amortized
          // syncWal, off the reactor thread)
  IdleSpin, // opt-in benchmark-parity spin threads (--fill-idle-cores); no
            // useful work, pins one core each with folly::asm_volatile_pause
            // to match Hypernode's "one perpetually spinning thread per
            // core" mpstat shape.
};

/// Human-readable name used as the OS thread-name prefix for a pool role.
const char* poolRoleName(PoolRole role);

/// Tracks the cumulative CPU time consumed by all threads of a single pool.
///
/// Each pool thread registers its per-thread CPU clock on startup and
/// unregisters on exit (folding its final CPU time into a retired total).
/// totalCpuNs() may be called concurrently from a monitoring thread to sample
/// live pools, which is what enables both per-interval deltas and a final
/// per-pool total for RCU projection.
class PoolCpuAccounting {
 public:
  void registerCurrentThread();
  void unregisterCurrentThread();
  uint64_t totalCpuNs() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::thread::id, clockid_t>> liveClocks_;
  uint64_t retiredCpuNs_{0};
};

/// A folly ThreadFactory that names threads, optionally pins each thread to a
/// core from a fixed core set (round-robin), and registers/unregisters every
/// thread with a PoolCpuAccounting instance. Pinning is skipped when the core
/// set is empty.
class CpuPinningThreadFactory : public folly::InitThreadFactory {
 public:
  CpuPinningThreadFactory(
      std::shared_ptr<folly::ThreadFactory> inner,
      std::vector<int> cores,
      PoolCpuAccounting* accounting);

 private:
  struct State {
    std::vector<int> cores;
    std::atomic<size_t> nextIndex{0};
    PoolCpuAccounting* accounting{nullptr};
  };

  CpuPinningThreadFactory(
      std::shared_ptr<folly::ThreadFactory> inner,
      std::shared_ptr<State> state);

  static std::shared_ptr<State> makeState(
      std::vector<int> cores,
      PoolCpuAccounting* accounting);
  static void onThreadStart(State& state);

  std::shared_ptr<State> state_;
};

/// Configuration for the set of Halcyon pools. Pool sizes default to 1 and are
/// clamped to >= 1 by buildPools(). Core lists are only applied when
/// pinEnabled is true.
struct PoolConfig {
  size_t qioThreads = 1;
  size_t qmetaThreads = 1;
  size_t qnetThreads = 1;
  size_t qflushThreads = 1;
  bool pinEnabled = false;
  // Reactor cores are consumed by makeReactorThread(), which spawns raw
  // std::threads outside any executor. Kept separate from qioCores so the
  // reactor factory has its own round-robin counter and does not share pin
  // slots with qioPool workers.
  std::vector<int> reactorCores;
  std::vector<int> qioCores;
  std::vector<int> qmetaCores;
  std::vector<int> qnetCores;
  std::vector<int> qflushCores;
  // Consumed by makeIdleSpinThread(). One idle-spin thread will be pinned
  // to each core in this list (round-robin); typically the caller fills
  // this with every core not otherwise claimed by a real pool.
  std::vector<int> idleSpinCores;
};

/// Owns the named, optionally pinned executor pools plus their CPU accounting.
/// The accounting objects are heap-allocated so their addresses stay stable
/// for the lifetime of the pools (the thread factory holds raw pointers).
///
/// Declaration order matters: the accounting objects are declared before the
/// pools so they are destroyed *after* the pools. Pool threads run their
/// finalizer (which calls PoolCpuAccounting::unregisterCurrentThread) as they
/// are joined during pool destruction, so the accounting must still be alive.
struct HalcyonPools {
  // Declared before the pools/factories (destroyed after them) so worker
  // threads can still unregister their CPU clocks as they join during
  // destruction.
  std::unique_ptr<PoolCpuAccounting> reactorCpu;
  std::unique_ptr<PoolCpuAccounting> qioCpu;
  std::unique_ptr<PoolCpuAccounting> qmetaCpu;
  std::unique_ptr<PoolCpuAccounting> qnetCpu;
  std::unique_ptr<PoolCpuAccounting> qflushCpu;
  std::unique_ptr<PoolCpuAccounting> idleSpinCpu;
  std::unique_ptr<folly::CPUThreadPoolExecutor> qioPool;
  // QMetadata lookup-worker pool (RocksDb backend only). Worker loops run as
  // long-lived tasks on this executor; null/empty when not using the pool.
  std::unique_ptr<folly::CPUThreadPoolExecutor> qmetaPool;
  // QNet network-send pool. Each synthetic-partner send worker
  // (NetworkOperator) runs as a long-lived task on this executor; one thread
  // per network worker.
  std::unique_ptr<folly::CPUThreadPoolExecutor> qnetPool;
  // QFlush chunk-map write-offload pool (RocksDb backend only). Flush workers
  // batch putBatch + one amortized syncWal per cycle off the reactor thread;
  // null/empty when not using the pool.
  std::unique_ptr<folly::CPUThreadPoolExecutor> qflushPool;

  /// Thread factory used only for reactor std::threads (makeReactorThread).
  /// Has its own core list and its own round-robin counter so reactors get
  /// dedicated pin slots and never compete for cores with qioPool workers.
  std::shared_ptr<folly::ThreadFactory> reactorThreadFactory;
  /// Thread factory backing the QIOThread executor pool.
  std::shared_ptr<folly::ThreadFactory> qioThreadFactory;
  /// Thread factory used only for --fill-idle-cores spin threads
  /// (makeIdleSpinThread). Pinned round-robin over PoolConfig::idleSpinCores.
  std::shared_ptr<folly::ThreadFactory> idleSpinThreadFactory;

  folly::CPUThreadPoolExecutor& pool(PoolRole role) const;
  uint64_t getCpuNs(PoolRole role) const;

  /// Spawns a raw std::thread that is named, optionally pinned, and registered
  /// with the Reactor pool's CPU accounting (via reactorThreadFactory). Used
  /// for the io_uring IO-Core reactor threads, which own their own event loop
  /// rather than running as CPUThreadPoolExecutor tasks.
  std::thread makeReactorThread(folly::Func func);

  /// Spawns a raw std::thread that is named, pinned, and CPU-accounted under
  /// the IdleSpin pool. Used for --fill-idle-cores spin threads; the caller
  /// supplies the busy-wait body (typically a `while (!stop) pause()` loop).
  std::thread makeIdleSpinThread(folly::Func func);
};

HalcyonPools buildPools(const PoolConfig& config);

} // namespace facebook::halcyon
