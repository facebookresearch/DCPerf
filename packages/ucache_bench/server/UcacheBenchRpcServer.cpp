// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "UcacheBenchRpcServer.h"

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

#include "CpuManager.h"
#include "UcacheBenchIOThreadContext.h"

DEFINE_uint32(rpc_io_threads, 0, "Number of IO threads for RPC server");
DEFINE_double(
    rpc_io_threads_multiplier,
    1.0,
    "Multiplier for IO thread count. Production ucache typically uses 0.75-1.0. "
    "Final thread count = base_threads * multiplier");
DEFINE_uint32(
    rpc_num_acceptor_threads,
    4,
    "Number of acceptor threads for handling new connections. "
    "Production ucache defaults to 4. Set to 0 to use CPU count");
DEFINE_uint32(
    rpc_num_cpu_worker_threads,
    1,
    "Number of CPU worker threads for ThriftServer. "
    "Production ucache uses 1. These handle CPU-bound work separate from IO");

// CPU pinning configuration flags
DEFINE_bool(
    cpu_pinning_enabled,
    false,
    "Enable CPU pinning for IO threads to reduce softirq overhead");
DEFINE_bool(
    cpu_pinning_avoid_irqs,
    true,
    "Avoid CPUs that handle NIC IRQs (reduces softirq contention)");
DEFINE_string(
    cpu_pinning_network_interface,
    "eth0",
    "Network interface name for IRQ detection");
DEFINE_bool(
    cpu_pinning_physical_cores_only,
    false,
    "Use only physical cores (skip hyperthreads)");
DEFINE_bool(
    cpu_pinning_exclusive,
    false,
    "Pin each thread to exactly one CPU (exclusive mode). "
    "Default (false) pins all threads to the same set of non-IRQ CPUs, "
    "allowing kernel scheduler to balance load for better performance");
DEFINE_bool(
    cpu_pinning_reduce_threads,
    true,
    "Reduce IO thread count to match non-IRQ CPU count when avoiding IRQs. "
    "This prevents thread oversubscription on remaining CPUs");

namespace facebook::ucachebench {

namespace {
constexpr size_t kDefaultNumThreads = 4;

CpuPinningOptions buildCpuPinningOptionsFromFlags() {
  CpuPinningOptions opts;
  opts.enabled = FLAGS_cpu_pinning_enabled;
  opts.avoidIrqs = FLAGS_cpu_pinning_avoid_irqs;
  opts.networkInterface = FLAGS_cpu_pinning_network_interface;
  opts.physicalCoresOnly = FLAGS_cpu_pinning_physical_cores_only;
  opts.exclusivePinning = FLAGS_cpu_pinning_exclusive;
  return opts;
}

} // namespace

UcacheBenchRpcServer::UcacheBenchRpcServer()
    : numThreads_(numIoThreads()),
      cpuPinningOpts_(buildCpuPinningOptionsFromFlags()) {}

UcacheBenchRpcServer::UcacheBenchRpcServer(
    const CpuPinningOptions& cpuPinningOpts)
    : numThreads_(numIoThreads()), cpuPinningOpts_(cpuPinningOpts) {}

UcacheBenchRpcServer::~UcacheBenchRpcServer() {
  stop();
}

size_t UcacheBenchRpcServer::numIoThreads() noexcept {
  size_t baseThreads = 0;

  if (FLAGS_rpc_io_threads > 0) {
    baseThreads = FLAGS_rpc_io_threads;
  } else {
    // Use CpuManager to get accurate CPU count (respects cgroups)
    size_t numCpus = CpuManager::getInstance().getNumCpus();
    if (numCpus == 0) {
      baseThreads = kDefaultNumThreads;
    } else {
      baseThreads = numCpus;
    }

    // Reduce thread count when avoiding IRQs to prevent oversubscription.
    // This matches production ucache behavior where thread count is reduced
    // by the number of IRQ CPUs being avoided.
    if (FLAGS_cpu_pinning_enabled && FLAGS_cpu_pinning_avoid_irqs &&
        FLAGS_cpu_pinning_reduce_threads) {
      auto irqCpus = CpuManager::getInstance().getIrqCpus(
          FLAGS_cpu_pinning_network_interface);
      size_t numIrqCpus = irqCpus.size();

      if (numIrqCpus > 0 && baseThreads > numIrqCpus) {
        size_t reducedThreads = baseThreads - numIrqCpus;
        // Ensure we still have at least half the CPUs as threads
        size_t minThreads = baseThreads / 2;
        if (reducedThreads >= minThreads) {
          XLOG(INFO) << "Reducing IO thread count from " << baseThreads
                     << " to " << reducedThreads << " (excluding " << numIrqCpus
                     << " IRQ CPUs)";
          baseThreads = reducedThreads;
        } else {
          XLOG(WARNING)
              << "Not reducing thread count: would go below minimum threshold ("
              << minThreads << ")";
        }
      }
    }
  }

  // Apply multiplier (matches production ucache behavior)
  // Production typically uses 0.75-1.0 multiplier
  size_t multipliedThreads =
      static_cast<size_t>(baseThreads * FLAGS_rpc_io_threads_multiplier);
  if (multipliedThreads > 0) {
    XLOG(INFO) << "IO thread count: " << multipliedThreads
               << " (base=" << baseThreads
               << " * multiplier=" << FLAGS_rpc_io_threads_multiplier << ")";
    return multipliedThreads;
  }

  return baseThreads;
}

apache::thrift::ThriftServer& UcacheBenchRpcServer::addThriftServer() {
  if (thriftServer_) {
    throw std::logic_error("ThriftServer already created");
  }

  thriftServer_ = std::make_unique<apache::thrift::ThriftServer>();

  // Configure CPU worker threads (matches production ucache which uses 1)
  thriftServer_->setNumCPUWorkerThreads(FLAGS_rpc_num_cpu_worker_threads);

  // Configure acceptor threads (production ucache defaults to 4)
  // Acceptor threads handle new connection accepts (TCP handshakes)
  uint32_t numAcceptorThreads = FLAGS_rpc_num_acceptor_threads;
  if (numAcceptorThreads == 0) {
    numAcceptorThreads = CpuManager::getInstance().getNumCpus();
  }
  thriftServer_->setNumAcceptThreads(numAcceptorThreads);

  XLOG(INFO) << "ThriftServer configured with "
             << FLAGS_rpc_num_cpu_worker_threads << " CPU worker threads and "
             << numAcceptorThreads << " acceptor threads";

  return *thriftServer_;
}

void UcacheBenchRpcServer::applyCpuPinning() {
  if (!cpuPinningOpts_.enabled) {
    XLOG(INFO) << "CPU pinning disabled, skipping";
    return;
  }

  auto evbs = extractIOEvbs();
  if (evbs.empty()) {
    XLOG(WARNING) << "No EventBases available for CPU pinning";
    return;
  }

  // Print diagnostics before applying pinning
  CpuManager::getInstance().printDiagnostics(cpuPinningOpts_.networkInterface);

  if (!CpuManager::getInstance().applyPinning(evbs, cpuPinningOpts_)) {
    XLOG(ERR) << "Failed to apply CPU pinning";
  } else {
    XLOG(INFO) << "Successfully applied CPU pinning with IRQ avoidance";
  }
}

void UcacheBenchRpcServer::start(
    const std::function<void(folly::EventBase&)>& threadInit,
    const std::function<void()>& threadCleanup) {
  if (!thriftServer_) {
    throw std::logic_error("No ThriftServer created");
  }

  // Create IO thread pool
  ioThreadPool_ = std::make_shared<folly::IOThreadPoolExecutor>(
      numThreads_,
      std::make_shared<folly::NamedThreadFactory>("UcacheBenchIO"));

  // Set up thread initialization with fiber context
  class CustomIOObserver : public folly::IOThreadPoolExecutorBase::IOObserver {
   public:
    CustomIOObserver(
        std::function<void(folly::EventBase&)> init,
        std::function<void()> cleanup)
        : threadInit_(std::move(init)), threadCleanup_(std::move(cleanup)) {}

    void registerEventBase(folly::EventBase& evb) override {
      // Initialize fiber context for this thread
      UcacheBenchIOThreadContext::init(evb);

      if (threadInit_) {
        threadInit_(evb);
      }
    }

    void unregisterEventBase(folly::EventBase& evb) override {
      if (threadCleanup_) {
        threadCleanup_();
      }
    }

   private:
    std::function<void(folly::EventBase&)> threadInit_;
    std::function<void()> threadCleanup_;
  };

  ioThreadPool_->addObserver(
      std::make_shared<CustomIOObserver>(threadInit, threadCleanup));

  thriftServer_->setIOThreadPool(ioThreadPool_);
  thriftServer_->setNumIOWorkerThreads(numThreads_);

  // Apply CPU pinning after thread pool is created
  applyCpuPinning();

  XLOG(INFO) << "UcacheBenchRpcServer IO thread pool created with "
             << numThreads_ << " threads";
}

void UcacheBenchRpcServer::serve() {
  if (!thriftServer_) {
    throw std::logic_error("No ThriftServer created");
  }

  // Start the server in a separate thread.
  // Note: This must be called AFTER setInterface() has been called on the
  // ThriftServer.
  serverRunner_ = std::thread([this]() { thriftServer_->serve(); });

  XLOG(INFO) << "UcacheBenchRpcServer started serving";
}

void UcacheBenchRpcServer::stop() {
  if (thriftServer_) {
    thriftServer_->stop();
  }

  if (serverRunner_.joinable()) {
    serverRunner_.join();
  }

  if (ioThreadPool_) {
    ioThreadPool_->stop();
    ioThreadPool_.reset();
  }
}

void UcacheBenchRpcServer::ensureAcceptorsShutdown() {
  if (thriftServer_) {
    thriftServer_->stopListening();
  }
}

std::shared_ptr<folly::IOThreadPoolExecutorBase>
UcacheBenchRpcServer::getThreadPoolExecutor() {
  return ioThreadPool_;
}

std::vector<folly::EventBase*> UcacheBenchRpcServer::extractIOEvbs() {
  std::vector<folly::EventBase*> evbs;
  if (ioThreadPool_) {
    auto allEvbs = ioThreadPool_->getAllEventBases();
    for (auto& evbPtr : allEvbs) {
      evbs.push_back(evbPtr.get());
    }
  }
  return evbs;
}

} // namespace facebook::ucachebench
