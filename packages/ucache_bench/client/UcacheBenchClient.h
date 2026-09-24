/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <folly/fibers/Semaphore.h>
#include <folly/io/async/EventBase.h>
#include <mcrouter/CarbonRouterClient.h>
#include <mcrouter/CarbonRouterInstance.h>
#include <mcrouter/options.h>

#ifdef OSS_BUILD
#include "UcacheBenchMessages.h"
#include "UcacheBenchRouterInfo.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchMessages.h"
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchRouterInfo.h"
#endif

namespace facebook {
namespace ucachebench {

struct MeasurementWindowNs {
  int64_t start;
  int64_t end;
};

constexpr MeasurementWindowNs immediateFailureWindow(int64_t nowNs) {
  return {.start = nowNs, .end = nowNs};
}

constexpr uint32_t kRampCoordinationSlackSeconds = 180;
constexpr size_t kMaxLatencySamples = 16'384;

constexpr uint64_t latencySamplePriority(
    uint64_t candidateIndex,
    size_t workerId,
    int32_t clientId) {
  uint64_t value = candidateIndex ^
      ((static_cast<uint64_t>(workerId) + 1) * 0x9e3779b97f4a7c15ULL) ^
      ((static_cast<uint64_t>(clientId + 2)) * 0xd1b54a32d192ed03ULL);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

using PrioritizedLatencySample = std::pair<uint64_t, double>;

struct alignas(64) WorkerLatencySampler {
  uint64_t candidates{0};
  std::vector<PrioritizedLatencySample> samples;
};

inline void recordPrioritizedLatencySample(
    std::vector<PrioritizedLatencySample>& samples,
    uint64_t priority,
    double latencyMs,
    size_t maxSamples = kMaxLatencySamples) {
  const auto priorityLess = [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  };
  if (samples.size() < maxSamples) {
    samples.emplace_back(priority, latencyMs);
    if (samples.size() == maxSamples) {
      std::make_heap(samples.begin(), samples.end(), priorityLess);
    }
    return;
  }
  if (priority >= samples.front().first) {
    return;
  }

  std::pop_heap(samples.begin(), samples.end(), priorityLess);
  samples.back() = {priority, latencyMs};
  std::push_heap(samples.begin(), samples.end(), priorityLess);
}

constexpr uint32_t rampCoordinationTimeoutSeconds(uint32_t rampSeconds) {
  return rampSeconds + kRampCoordinationSlackSeconds;
}

constexpr uint64_t processRampDelayNs(
    int32_t clientId,
    uint32_t clientCount,
    uint32_t rampSeconds) {
  return static_cast<uint64_t>(clientId - 1) * rampSeconds * 1000000000ULL /
      clientCount;
}

constexpr uint64_t countSequenceIntersection(
    uint64_t begin,
    uint64_t end,
    uint64_t measurementBegin,
    uint64_t measurementEnd) {
  const uint64_t intersectionBegin = std::max(begin, measurementBegin);
  const uint64_t intersectionEnd = std::min(end, measurementEnd);
  return intersectionEnd > intersectionBegin
      ? intersectionEnd - intersectionBegin
      : 0;
}

constexpr std::optional<int64_t> alignWallTimeToSteadyClock(
    int64_t targetWallNs,
    int64_t wallNowNs,
    int64_t steadyNowNs) {
  const __int128 delta =
      static_cast<__int128>(targetWallNs) - static_cast<__int128>(wallNowNs);
  const __int128 aligned = static_cast<__int128>(steadyNowNs) + delta;
  if (delta <= 0 || aligned < std::numeric_limits<int64_t>::min() ||
      aligned > std::numeric_limits<int64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<int64_t>(aligned);
}

constexpr bool shouldUsePerRequestTimeout(bool openLoopEnabled) {
  return !openLoopEnabled;
}

namespace detail {

class PhysicalOutstandingTracker;

class PhysicalOutstandingLease final {
 public:
  explicit PhysicalOutstandingLease(
      std::shared_ptr<PhysicalOutstandingTracker> tracker)
      : tracker_(std::move(tracker)) {}
  PhysicalOutstandingLease(const PhysicalOutstandingLease&) = delete;
  PhysicalOutstandingLease& operator=(const PhysicalOutstandingLease&) = delete;
  PhysicalOutstandingLease(PhysicalOutstandingLease&&) = delete;
  PhysicalOutstandingLease& operator=(PhysicalOutstandingLease&&) = delete;
  ~PhysicalOutstandingLease() noexcept;

  void release() noexcept;

 private:
  std::shared_ptr<PhysicalOutstandingTracker> tracker_;
  std::atomic<bool> released_{false};
};

class PhysicalOutstandingTracker final
    : public std::enable_shared_from_this<PhysicalOutstandingTracker> {
 public:
  std::shared_ptr<PhysicalOutstandingLease> acquire() {
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    try {
      return std::make_shared<PhysicalOutstandingLease>(shared_from_this());
    } catch (...) {
      release();
      throw;
    }
  }

  std::shared_ptr<PhysicalOutstandingLease> tryAcquire(uint32_t limit) {
    uint32_t outstanding = outstanding_.load(std::memory_order_relaxed);
    while (outstanding < limit) {
      if (outstanding_.compare_exchange_weak(
              outstanding,
              outstanding + 1,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        try {
          return std::make_shared<PhysicalOutstandingLease>(shared_from_this());
        } catch (...) {
          release();
          throw;
        }
      }
    }
    return nullptr;
  }

  uint32_t outstanding() const {
    return outstanding_.load(std::memory_order_relaxed);
  }

 private:
  friend class PhysicalOutstandingLease;

  void release() {
    outstanding_.fetch_sub(1, std::memory_order_relaxed);
  }

  std::atomic<uint32_t> outstanding_{0};
};

inline void PhysicalOutstandingLease::release() noexcept {
  if (!released_.exchange(true, std::memory_order_relaxed)) {
    tracker_->release();
  }
}

inline PhysicalOutstandingLease::~PhysicalOutstandingLease() noexcept {
  release();
}

} // namespace detail

constexpr bool shouldRefillGetMiss(
    bool openLoopEnabled,
    bool refillOnMissEnabled) {
  return !openLoopEnabled || refillOnMissEnabled;
}

constexpr std::optional<int64_t> checkedMeasurementEndNs(
    int64_t startNs,
    uint32_t durationSeconds) {
  const __int128 end = static_cast<__int128>(startNs) +
      static_cast<__int128>(durationSeconds) * 1000000000;
  if (end > std::numeric_limits<int64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<int64_t>(end);
}

constexpr bool
isInMeasurementWindow(int64_t valueNs, int64_t startNs, int64_t endNs) {
  return valueNs >= startNs && valueNs < endNs;
}

// Zipfian distribution generator for realistic hot-key access patterns
// Based on YCSB's ScrambledZipfianGenerator algorithm
class ZipfianGenerator {
 public:
  explicit ZipfianGenerator(uint64_t numItems, double skew = 0.99);

  // Generate a Zipfian-distributed random number in [0, numItems)
  uint64_t next();

  // Get the skew parameter
  double getSkew() const {
    return skew_;
  }

 private:
  uint64_t numItems_;
  double skew_;
  double zetan_; // Normalization constant
  double eta_; // Precomputed value for fast sampling
  double theta_; // = skew
  double alpha_; // = 1 / (1 - theta)
  double zetaTwo_; // zeta(2, theta)

  // Compute zeta(n, theta) = sum_{i=1}^{n} 1/i^theta
  static double zeta(uint64_t n, double theta);
};

/**
 * Admin server connection for multi-client coordination.
 * Connects to the server's admin port to participate in phase synchronization.
 */
class AdminConnection {
 public:
  AdminConnection() = default;
  ~AdminConnection();

  // Connect to the admin server
  bool connect(
      const std::string& host,
      uint16_t port,
      uint32_t receiveTimeoutSeconds);

  // Interrupt a blocking notification read. The caller must join the reader
  // before disconnecting and clearing its buffers.
  void requestCancellation();

  // Disconnect from the admin server
  void disconnect();

  // Check if connected
  bool isConnected() const {
    return socket_ >= 0;
  }

  // Send REGISTER command and get assigned client ID
  // Returns the assigned client ID, or -1 on error
  int32_t sendRegister(uint32_t protocolVersion = 1);

  // Coordinate protocol-v2 process ramp and measurement start.
  bool sendRampReady(int32_t clientId, uint32_t durationSeconds);
  bool sendRampStarted(int32_t clientId);

  // Send WARMUP_DONE command
  bool sendWarmupDone(int32_t clientId);

  // Send BENCHMARK_DONE command
  bool sendBenchmarkDone(int32_t clientId);

  // Wait for an async notification from the server
  // Returns the notification message, or empty string on error/timeout
  std::string waitForNotification(uint32_t timeoutSeconds = 0);

 private:
  // Send a command and receive response
  // Filters out broadcast notifications and buffers them for later retrieval
  std::string sendCommand(const std::string& command);

  // Read a line from the socket
  std::string readLine();

  // Check if a message is a broadcast notification (vs a command response)
  static bool isBroadcastNotification(const std::string& message);

  std::atomic<int> socket_{-1};
  uint32_t receiveTimeoutSeconds_{600};
  std::string readBuffer_;
  // Buffer for broadcast notifications received while waiting for command
  // response
  std::vector<std::string> pendingNotifications_;
};

class UcacheBenchClient {
 public:
  // Production traffic distribution configuration
  struct TrafficDistribution {
    double getRatio{0.9};
    // GET operation sizes
    double getKeySizeAvg{64.0};
    double getResponseSizeAvg{1000.0};
    double getResponseSizeP50{50.0};
    double getResponseSizeP75{250.0};
    double getResponseSizeP95{2000.0};
    double getResponseSizeP99{10000.0};
    // SET operation sizes
    double setKeySizeAvg{68.0};
    double setValueSizeAvg{1400.0};
    double setValueSizeP50{50.0};
    double setValueSizeP75{100.0};
    double setValueSizeP95{2000.0};
    double setValueSizeP99{20000.0};
    bool enabled{false}; // Whether to use distribution mode
  };

  struct WarmupResults {
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;
    uint64_t totalOps{0};
    uint64_t setSuccesses{0};
    uint64_t setErrors{0};
    uint64_t tailSetErrors{0};
    bool success{false};
  };

  struct BenchmarkResults {
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point endTime;
    uint64_t totalOps{0};
    uint64_t getOps{0};
    uint64_t setOps{0};
    uint64_t getHits{0};
    uint64_t getMisses{0};
    uint64_t getErrors{0};
    uint64_t setSuccesses{0};
    uint64_t setErrors{0};
    std::vector<double> latencies;
    WarmupResults warmupResults; // Include warmup results
  };

  UcacheBenchClient();
  ~UcacheBenchClient();

  WarmupResults warmup();
  BenchmarkResults runBenchmark();
  void printResults(const BenchmarkResults& results);

  // Admin server coordination
  // If admin server is configured, the client will:
  // 1. Connect and register to get a client ID
  // 2. Wait for ALL_REGISTERED before starting warmup
  // 3. Send WARMUP_DONE and wait for ALL_WARMUP_DONE before benchmark
  // 4. Send BENCHMARK_DONE after completing benchmark

  // Connect to admin server (called from main if --admin_port is set)
  // Uses server_host since admin server runs on the same machine as cache
  // server
  bool connectToAdmin(const std::string& host, uint16_t port);

  // Check if admin connection is active
  bool hasAdminConnection() const {
    return adminConnection_ && adminConnection_->isConnected();
  }

  // Get the client ID assigned by the admin server
  int32_t getClientId() const {
    return clientId_;
  }

 private:
  std::string generateKey();
  std::string generateValue();

  // Production traffic distribution support
  void loadTrafficDistribution(const std::string& configFile);
  uint32_t sampleFromPercentiles(double p50, double p75, double p95, double p99)
      const;

  TrafficDistribution distribution_;

  // mcrouter operations using UcacheBench service
  // These methods now accept a client pointer for per-thread client usage
  void sendUcbGetRequestSync(
      facebook::memcache::mcrouter::CarbonRouterClient<
          UcacheBenchRouterInfo>::Pointer& client,
      const std::string& key,
      const std::function<void(UcbGetReply&&)>& callback);
  void sendUcbSetRequestSync(
      facebook::memcache::mcrouter::CarbonRouterClient<
          UcacheBenchRouterInfo>::Pointer& client,
      const std::string& key,
      const std::string& value,
      const std::function<void(UcbSetReply&&)>& callback);

  // mcrouter client and connection management
  std::shared_ptr<
      facebook::memcache::mcrouter::CarbonRouterInstance<UcacheBenchRouterInfo>>
      routerInstance_;

  // Effective connection parameters (derived from --num_connections or flags)
  uint32_t effectiveNumProxies_{0};
  uint32_t effectiveAdditionalFanout_{0};

  // Admin server connection for multi-client coordination
  std::unique_ptr<AdminConnection> adminConnection_;
  int32_t clientId_{-1}; // Assigned by admin server
};

} // namespace ucachebench
} // namespace facebook
