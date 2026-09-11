// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ThreadPools.h"

#include <sched.h>
#include <algorithm>

#include <folly/executors/thread_factory/NamedThreadFactory.h>

namespace facebook::halcyon {

namespace {

uint64_t readClockNs(clockid_t clockId) {
  timespec ts{};
  if (clock_gettime(clockId, &ts) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
      static_cast<uint64_t>(ts.tv_nsec);
}

std::shared_ptr<CpuPinningThreadFactory> makeCpuPinningFactory(
    folly::StringPiece name,
    std::vector<int> cores,
    PoolCpuAccounting* accounting) {
  return std::make_shared<CpuPinningThreadFactory>(
      std::make_shared<folly::NamedThreadFactory>(name),
      std::move(cores),
      accounting);
}

std::unique_ptr<folly::CPUThreadPoolExecutor> makePool(
    size_t numThreads,
    std::shared_ptr<folly::ThreadFactory> factory) {
  return std::make_unique<folly::CPUThreadPoolExecutor>(
      std::max<size_t>(1, numThreads), std::move(factory));
}

} // namespace

const char* poolRoleName(PoolRole role) {
  switch (role) {
    case PoolRole::Reactor:
      return "Reactor";
    case PoolRole::QIOThread:
      return "QIOThread";
    case PoolRole::QMetadata:
      return "QMetadata";
    case PoolRole::QNet:
      return "QNet";
    case PoolRole::QFlush:
      return "QFlush";
    case PoolRole::IdleSpin:
      return "IdleSpin";
  }
  return "Unknown";
}

void PoolCpuAccounting::registerCurrentThread() {
  clockid_t clockId{};
  if (pthread_getcpuclockid(pthread_self(), &clockId) != 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  liveClocks_.emplace_back(std::this_thread::get_id(), clockId);
}

void PoolCpuAccounting::unregisterCurrentThread() {
  const auto id = std::this_thread::get_id();
  std::lock_guard<std::mutex> guard(mutex_);
  for (auto it = liveClocks_.begin(); it != liveClocks_.end(); ++it) {
    if (it->first == id) {
      retiredCpuNs_ += readClockNs(it->second);
      liveClocks_.erase(it);
      return;
    }
  }
}

uint64_t PoolCpuAccounting::totalCpuNs() const {
  std::lock_guard<std::mutex> guard(mutex_);
  uint64_t total = retiredCpuNs_;
  for (const auto& [id, clockId] : liveClocks_) {
    total += readClockNs(clockId);
  }
  return total;
}

std::shared_ptr<CpuPinningThreadFactory::State>
CpuPinningThreadFactory::makeState(
    std::vector<int> cores,
    PoolCpuAccounting* accounting) {
  auto state = std::make_shared<State>();
  state->cores = std::move(cores);
  state->accounting = accounting;
  return state;
}

void CpuPinningThreadFactory::onThreadStart(State& state) {
  if (!state.cores.empty()) {
    const size_t index =
        state.nextIndex.fetch_add(1, std::memory_order_relaxed) %
        state.cores.size();
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(state.cores[index], &cpuSet);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuSet), &cpuSet);
  }
  state.accounting->registerCurrentThread();
}

CpuPinningThreadFactory::CpuPinningThreadFactory(
    std::shared_ptr<folly::ThreadFactory> inner,
    std::vector<int> cores,
    PoolCpuAccounting* accounting)
    : CpuPinningThreadFactory(
          std::move(inner),
          makeState(std::move(cores), accounting)) {}

CpuPinningThreadFactory::CpuPinningThreadFactory(
    std::shared_ptr<folly::ThreadFactory> inner,
    std::shared_ptr<State> state)
    : folly::InitThreadFactory(
          std::move(inner),
          [state] { onThreadStart(*state); },
          [state] { state->accounting->unregisterCurrentThread(); }),
      state_(std::move(state)) {}

folly::CPUThreadPoolExecutor& HalcyonPools::pool(PoolRole role) const {
  switch (role) {
    case PoolRole::Reactor:
      // Reactors run as raw std::threads, not on an executor. Fall through
      // to the QIO pool so callers that ask by role still get a usable
      // executor (used for background offload adjacent to reactor work).
      return *qioPool;
    case PoolRole::QIOThread:
      return *qioPool;
    case PoolRole::QMetadata:
      return *qmetaPool;
    case PoolRole::QNet:
      return *qnetPool;
    case PoolRole::QFlush:
      return *qflushPool;
    case PoolRole::IdleSpin:
      // IdleSpin threads are raw std::threads via makeIdleSpinThread; no
      // executor. Fall through to qioPool so callers that ask by role get
      // a valid reference (they should never enqueue work on it).
      return *qioPool;
  }
  return *qioPool;
}

uint64_t HalcyonPools::getCpuNs(PoolRole role) const {
  switch (role) {
    case PoolRole::Reactor:
      return reactorCpu ? reactorCpu->totalCpuNs() : 0;
    case PoolRole::QIOThread:
      return qioCpu->totalCpuNs();
    case PoolRole::QMetadata:
      return qmetaCpu ? qmetaCpu->totalCpuNs() : 0;
    case PoolRole::QNet:
      return qnetCpu ? qnetCpu->totalCpuNs() : 0;
    case PoolRole::QFlush:
      return qflushCpu ? qflushCpu->totalCpuNs() : 0;
    case PoolRole::IdleSpin:
      return idleSpinCpu ? idleSpinCpu->totalCpuNs() : 0;
  }
  return 0;
}

std::thread HalcyonPools::makeReactorThread(folly::Func func) {
  return reactorThreadFactory->newThread(std::move(func));
}

std::thread HalcyonPools::makeIdleSpinThread(folly::Func func) {
  return idleSpinThreadFactory->newThread(std::move(func));
}

HalcyonPools buildPools(const PoolConfig& config) {
  HalcyonPools pools;
  pools.reactorCpu = std::make_unique<PoolCpuAccounting>();
  pools.qioCpu = std::make_unique<PoolCpuAccounting>();
  pools.qmetaCpu = std::make_unique<PoolCpuAccounting>();
  pools.qnetCpu = std::make_unique<PoolCpuAccounting>();
  pools.qflushCpu = std::make_unique<PoolCpuAccounting>();
  pools.idleSpinCpu = std::make_unique<PoolCpuAccounting>();

  pools.reactorThreadFactory = makeCpuPinningFactory(
      poolRoleName(PoolRole::Reactor),
      config.pinEnabled ? config.reactorCores : std::vector<int>{},
      pools.reactorCpu.get());

  pools.idleSpinThreadFactory = makeCpuPinningFactory(
      poolRoleName(PoolRole::IdleSpin),
      config.pinEnabled ? config.idleSpinCores : std::vector<int>{},
      pools.idleSpinCpu.get());

  pools.qioThreadFactory = makeCpuPinningFactory(
      poolRoleName(PoolRole::QIOThread),
      config.pinEnabled ? config.qioCores : std::vector<int>{},
      pools.qioCpu.get());
  pools.qioPool = makePool(config.qioThreads, pools.qioThreadFactory);

  pools.qmetaPool = makePool(
      config.qmetaThreads,
      makeCpuPinningFactory(
          poolRoleName(PoolRole::QMetadata),
          config.pinEnabled ? config.qmetaCores : std::vector<int>{},
          pools.qmetaCpu.get()));

  pools.qnetPool = makePool(
      config.qnetThreads,
      makeCpuPinningFactory(
          poolRoleName(PoolRole::QNet),
          config.pinEnabled ? config.qnetCores : std::vector<int>{},
          pools.qnetCpu.get()));

  pools.qflushPool = makePool(
      config.qflushThreads,
      makeCpuPinningFactory(
          poolRoleName(PoolRole::QFlush),
          config.pinEnabled ? config.qflushCores : std::vector<int>{},
          pools.qflushCpu.get()));
  return pools;
}

} // namespace facebook::halcyon
