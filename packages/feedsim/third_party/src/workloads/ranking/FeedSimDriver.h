// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct event_base;

namespace feedsim {

class DriverConnection;

/**
 * DriverStats: latency sampling and request counting.
 * Replaces oldisim::ChildConnectionStats + LogHistogramSampler.
 * Produces the exact same printf output format that run.sh/search_qps.sh parse.
 */
class DriverStats {
 public:
  explicit DriverStats(int bins = 1000);

  void logRequest(uint32_t type, uint32_t packet_size);
  void logResponse(uint32_t type, uint64_t latency_ns, uint32_t packet_size);
  void accumulate(const DriverStats& other);
  void reset();

  uint64_t getQueryCount(uint32_t type) const;
  uint64_t getStartTimeNano() const { return start_time_; }
  uint64_t getEndTimeNano() const { return end_time_; }
  void setEndTimeNano(uint64_t t) { end_time_ = t; }

  // Print stats in the exact format oldisim::DriverNode produces.
  // search_qps.sh and feedsim parser depend on this format.
  void printStats(uint32_t type, double elapsed_secs) const;

 private:
  struct LatencySampler;
  std::unique_ptr<LatencySampler> sampler_;
  uint64_t start_time_;
  uint64_t end_time_;
  // Per-type counters
  uint64_t tx_bytes_ = 0;
  uint64_t rx_bytes_ = 0;
  uint64_t query_count_ = 0;
};

/**
 * TestDriver: manages connections to the server and issues requests.
 * Replaces oldisim::TestDriver.
 */
class TestDriver {
 public:
  void sendRequest(uint32_t type, const void* payload, uint32_t length,
                   uint64_t next_request_delay_us);
  const DriverStats& getConnectionStats() const;
  event_base* getEventBase() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  TestDriver();
  friend class FeedSimDriver;
};

// Callback types for driver
using DriverThreadStartupCallback =
    std::function<void(int thread_id, TestDriver& driver)>;
using DriverMakeRequestCallback =
    std::function<void(int thread_id, TestDriver& driver)>;
using DriverResponseCallback =
    std::function<void(int thread_id, uint32_t type, const void* payload,
                       uint32_t length)>;

/**
 * FeedSimDriver replaces oldisim::DriverNode.
 *
 * Produces the same stats output format:
 *   Stats for node under test, type N
 *      RX: %.2f MB/sec (%lu bytes)
 *      TX: %.2f MB/sec (%lu bytes)
 *       #: %.2f QPS (%lu queries)
 *     min: %.3f ms
 *     avg: %.3f ms
 *     50p: %.3f ms
 *     ...
 *     99.9p: %.3f ms
 */
class FeedSimDriver {
 public:
  FeedSimDriver(const std::string& hostname, uint16_t port);
  ~FeedSimDriver();

  void setThreadStartupCallback(DriverThreadStartupCallback cb);
  void setMakeRequestCallback(DriverMakeRequestCallback cb);
  void registerReplyCallback(uint32_t type, DriverResponseCallback cb);
  void registerRequestType(uint32_t type);
  void enableMonitoring(uint16_t port);

  void run(uint32_t num_threads, bool thread_pinning,
           uint32_t num_connections_per_thread, uint32_t max_connection_depth);
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace feedsim
