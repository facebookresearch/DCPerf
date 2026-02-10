/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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

// Zipfian distribution generator for realistic hot-key access patterns
// Based on YCSB's ScrambledZipfianGenerator algorithm
class ZipfianGenerator {
 public:
  ZipfianGenerator(uint64_t numItems, double skew = 0.99);

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
  bool connect(const std::string& host, uint16_t port);

  // Disconnect from the admin server
  void disconnect();

  // Check if connected
  bool isConnected() const {
    return socket_ >= 0;
  }

  // Send REGISTER command and get assigned client ID
  // Returns the assigned client ID, or -1 on error
  int32_t sendRegister();

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

  int socket_{-1};
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
  // Note: No longer using a shared client_ member
  // Instead, each thread creates its own client from routerInstance_

  // Admin server connection for multi-client coordination
  std::unique_ptr<AdminConnection> adminConnection_;
  int32_t clientId_{-1}; // Assigned by admin server
};

} // namespace ucachebench
} // namespace facebook
