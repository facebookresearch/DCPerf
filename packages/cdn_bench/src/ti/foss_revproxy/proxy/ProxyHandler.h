/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/experimental/coro/Task.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "proxygen/lib/http/coro/HTTPCoroSession.h"
#include "proxygen/lib/http/coro/client/HTTPCoroSessionPool.h"
#include "ti/foss_revproxy/proxy/LoadBalancer.h"

namespace ti {
namespace foss_revproxy {

// Backend configuration structure
struct Backend {
  std::string host;
  uint16_t port;
  bool tls;
};

// Configuration for proxy behavior
struct ProxyConfig {
  bool enableDirectResponse{false};
  bool backendH2{false}; // Use HTTP/2 for plaintext backend connections
};

// Metrics collected by the proxy - this is the key benchmarking data
struct ProxyMetrics {
  std::atomic<uint64_t> requestsReceived{0};
  std::atomic<uint64_t> requestsSucceeded{0};
  std::atomic<uint64_t> requestsFailed{0};
  std::atomic<uint64_t> retriesAttempted{0};
  std::atomic<uint64_t> retriesSucceeded{0};
  std::atomic<uint64_t> totalLatencyUs{
      0}; // Total processing time in microseconds
  std::atomic<uint64_t> backendLatencyUs{0}; // Time waiting for backend
  std::chrono::steady_clock::time_point startTime{
      std::chrono::steady_clock::now()};

  void reset() {
    requestsReceived = 0;
    requestsSucceeded = 0;
    requestsFailed = 0;
    retriesAttempted = 0;
    retriesSucceeded = 0;
    totalLatencyUs = 0;
    backendLatencyUs = 0;
    startTime = std::chrono::steady_clock::now();
  }

  double getElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - startTime).count();
  }

  double getSuccessRate() const {
    uint64_t total = requestsSucceeded + requestsFailed;
    return total > 0 ? (100.0 * requestsSucceeded / total) : 0.0;
  }

  double getActualRPS() const {
    double elapsed = getElapsedSeconds();
    return elapsed > 0 ? (requestsSucceeded + requestsFailed) / elapsed : 0.0;
  }

  double getAvgLatencyMs() const {
    uint64_t total = requestsSucceeded + requestsFailed;
    return total > 0 ? (totalLatencyUs / total) / 1000.0 : 0.0;
  }

  double getAvgBackendLatencyMs() const {
    return requestsSucceeded > 0
        ? (backendLatencyUs / requestsSucceeded) / 1000.0
        : 0.0;
  }
};

/**
 * ProxyHandler - Core HTTP proxy request handler
 */
class ProxyHandler : public proxygen::coro::HTTPHandler {
 public:
  /**
   * Create a proxy handler with full configuration
   * @param backends List of backend servers to proxy to
   * @param loadBalancer Algorithm for selecting backends
   * @param config Proxy configuration (h2 support, retries, etc.)
   * @param metrics Optional shared metrics object for tracking performance
   */
  ProxyHandler(
      std::vector<Backend> backends,
      std::shared_ptr<LoadBalancer> loadBalancer,
      ProxyConfig config,
      std::shared_ptr<ProxyMetrics> metrics = nullptr);

  /**
   * Handle an incoming HTTP request
   *
   * This is called by the HTTPServer framework for each request.
   * Returns an HTTPSourceHolder representing the response.
   *
   * Gets backend connection pools via getBackendPool() function which
   * lazily creates pools per (EventBase, backend) pair.
   */
  folly::coro::Task<proxygen::coro::HTTPSourceHolder> handleRequest(
      folly::EventBase* evb,
      proxygen::coro::HTTPSessionContextPtr ctx,
      proxygen::coro::HTTPSourceHolder requestSource) override;

  /**
   * Get the metrics object for this handler
   */
  std::shared_ptr<ProxyMetrics> getMetrics() const {
    return metrics_;
  }

 private:
  std::vector<Backend> backends_;
  std::shared_ptr<LoadBalancer> loadBalancer_;
  ProxyConfig config_;
  std::shared_ptr<ProxyMetrics> metrics_;

  /**
   * Get or create connection pool for a specific backend and EventBase.
   *
   * Uses static pool storage with lazy initialization per (EventBase, backend)
   * pair. Returns reference for direct use.
   */
  proxygen::coro::HTTPCoroSessionPool& getBackendPool(
      folly::EventBase* evb,
      size_t backendIdx);

  /**
   * Forward request to a backend server
   */
  folly::coro::Task<proxygen::coro::HTTPSourceHolder> forwardToBackend(
      folly::EventBase* evb,
      std::unique_ptr<proxygen::HTTPMessage> headers,
      proxygen::coro::HTTPSourceHolder requestSource,
      std::chrono::steady_clock::time_point requestStart);

  /**
   * Send a direct response without going to backend
   */
  proxygen::coro::HTTPSourceHolder getDirectResponse(
      int statusCode,
      const std::string& body = "");

  /**
   * Record a failed request in metrics
   */
  void recordFailure(std::chrono::steady_clock::time_point requestStart);

  /**
   * Record a successful request in metrics
   */
  void recordSuccess(
      std::chrono::steady_clock::time_point requestStart,
      std::chrono::steady_clock::time_point backendStart);
};

} // namespace foss_revproxy
} // namespace ti
