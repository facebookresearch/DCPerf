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
};

} // namespace ucachebench
} // namespace facebook
