/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/experimental/coro/Sleep.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>

#include <atomic>
#include <chrono>
#include <random>
#include <vector>

#include "proxygen/httpserver/samples/hq/InsecureVerifierDangerousDoNotUseInProduction.h"
#include "proxygen/lib/http/coro/HTTPCoroSession.h"
#include "proxygen/lib/http/coro/HTTPFixedSource.h"
#include "proxygen/lib/http/coro/client/HTTPCoroConnector.h"
#include "proxygen/lib/http/coro/client/HTTPCoroSessionPool.h"

using namespace proxygen;
using namespace proxygen::coro;

// Constants
namespace {
constexpr size_t IO_BUFFER_SIZE = 4096; // Buffer size for reading responses
} // namespace

// Configuration flags
DEFINE_string(target_host, "::1", "Target server hostname or IP");
DEFINE_int32(target_port, 8081, "Target server port");
DEFINE_bool(target_tls, false, "Use TLS to connect to target");
DEFINE_bool(quic, false, "Use QUIC/HTTP3");

DEFINE_int32(target_rps, 10, "Target requests per second");
DEFINE_int32(duration_sec, 10, "Duration to run traffic (seconds)");
DEFINE_int32(max_requests, 0, "Max requests to send (0 = unlimited)");

DEFINE_int32(num_connections, 1, "Number of concurrent connections");
DEFINE_int32(
    streams_per_connection,
    1,
    "Max concurrent streams per connection");

DEFINE_double(
    reset_probability,
    0.0,
    "Probability (0.0-1.0) of resetting requests");

// Test URLs to cycle through
static const std::vector<std::string> TEST_URLS = {
    "/index.html",
    "/api/data.json",
    "/app.js",
    "/image.png",
    "/api/users",
    "/"};

namespace {
struct Metrics {
  std::atomic<uint64_t> requestsSent{0};
  std::atomic<uint64_t> responsesReceived{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> resets{0};
  std::chrono::steady_clock::time_point startTime;
};
} // namespace

std::string getRandomUrl() {
  thread_local std::mt19937 rng(std::random_device{}());
  thread_local std::uniform_int_distribution<size_t> dist(
      0, TEST_URLS.size() - 1);
  return TEST_URLS[dist(rng)];
}

/**
 * Sends a single HTTP request and awaits a response, potentially either
 * resetting the connection or reading the full response body. Probability is
 * controlled by reset_probability flag.
 */
folly::coro::Task<void> sendSingleRequest(
    HTTPCoroSessionPool& pool,
    Metrics& metrics,
    const uint64_t requestNum,
    const double resetProb) {
  std::string url = getRandomUrl();

  // Get connection from pool
  auto res = co_await co_awaitTry(pool.getSessionWithReservation());
  if (res.hasException()) {
    XLOG(ERR) << "Req #" << requestNum
              << " - Failed to get connection: " << res.exception().what();
    metrics.errors++;
    co_return;
  }

  // Create request
  auto request = HTTPFixedSource::makeFixedRequest(url, HTTPMethod::GET);
  request->msg_->getHeaders().set(HTTP_HEADER_HOST, FLAGS_target_host);

  XLOG(DBG4) << "Req #" << requestNum << " - Sending: " << url;
  metrics.requestsSent++;

  // Send request
  auto responseSource = co_await co_awaitTry(res->session->sendRequest(
      std::move(request), std::move(res->reservation)));

  if (responseSource.hasException()) {
    XLOG(ERR) << "Req #" << requestNum
              << " - Send failed: " << responseSource.exception().what();
    metrics.errors++;
    co_return;
  }

  // Check if we should reset
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (resetProb > 0 && dist(rng) < resetProb) {
    XLOG(INFO) << "Req #" << requestNum << " - Resetting";
    metrics.resets++;
    // Just return without reading response (simulates reset)
    co_return;
  }

  // Read response
  auto headerEvent = co_await co_awaitTry(responseSource->readHeaderEvent());
  if (headerEvent.hasException()) {
    XLOG(ERR) << "Req #" << requestNum << " - Failed to read headers";
    metrics.errors++;
    co_return;
  }

  uint16_t status = headerEvent->headers->getStatusCode();
  XLOG(DBG4) << "Req #" << requestNum << " - Status: " << status;

  // Drain response body
  if (!headerEvent->eom) {
    while (true) {
      auto bodyEvent = co_await responseSource->readBodyEvent(IO_BUFFER_SIZE);
      if (bodyEvent.eom) {
        break;
      }
    }
  }

  metrics.responsesReceived++;
}

/**
 * Worker that sends requests at a controlled rate
 */
folly::coro::Task<void> createTrafficWorkerTask(
    HTTPCoroSessionPool& pool,
    Metrics& metrics,
    std::atomic<bool>& shouldStop,
    const uint64_t workerNum) {
  XLOG(INFO) << "Worker " << workerNum << " started";

  uint64_t requestNum = 0;
  auto sleepDuration = std::chrono::milliseconds(
      1000 * FLAGS_num_connections * FLAGS_streams_per_connection /
      FLAGS_target_rps);

  while (!shouldStop.load()) {
    // Launch concurrent streams
    std::vector<folly::coro::Task<void>> tasks;
    tasks.reserve(FLAGS_streams_per_connection);
    for (int i = 0; i < FLAGS_streams_per_connection && !shouldStop.load();
         ++i) {
      tasks.push_back(sendSingleRequest(
          pool,
          metrics,
          workerNum * 1000000 + requestNum++,
          FLAGS_reset_probability));
    }

    // Wait for all streams to complete
    if (!tasks.empty()) {
      co_await folly::coro::collectAllRange(std::move(tasks));
    }

    // Rate limiting
    if (!shouldStop.load()) {
      co_await folly::coro::sleep(sleepDuration);
    }

    // Check max requests
    if (FLAGS_max_requests > 0 &&
        metrics.requestsSent >= static_cast<uint64_t>(FLAGS_max_requests)) {
      break;
    }
  }

  XLOG(INFO) << "Worker " << workerNum << " stopped";
}

/**
 * Run all workers and cleanup when complete
 */
folly::coro::Task<void> runAllWorkers(
    std::vector<folly::coro::Task<void>> tasks,
    std::atomic<bool>* shouldStopPtr,
    folly::EventBase* evbPtr) {
  co_await folly::coro::collectAllRange(std::move(tasks));
  XLOG(INFO) << "All workers completed";
  shouldStopPtr->store(true);
  evbPtr->terminateLoopSoon();
}

/**
 * Get or create connection pool (singleton per EventBase)
 */
std::unique_ptr<HTTPCoroSessionPool> createConnectionPool(
    folly::EventBase* evb) {
  HTTPCoroSessionPool::PoolParams poolParams;
  poolParams.maxConnections = FLAGS_num_connections * 2;

  if (FLAGS_quic) {
    // QUIC/HTTP3 configuration
    auto quicConnParams =
        std::make_shared<HTTPCoroConnector::QuicConnectionParams>();

    if (FLAGS_target_tls) {
      // Configure TLS for QUIC with "h3" ALPN
      auto fizzContext = HTTPCoroConnector::makeFizzClientContext(
          HTTPCoroConnector::defaultQuicTLSParams());

      // Use insecure verifier for test certificates
      // WARNING: This is insecure and should only be used for testing!
      auto insecureVerifier = std::make_shared<
          proxygen::InsecureVerifierDangerousDoNotUseInProduction>();
      quicConnParams->fizzContextAndVerifier = {
          std::move(fizzContext), std::move(insecureVerifier)};
      quicConnParams->serverName = FLAGS_target_host;
    }

    return std::make_unique<HTTPCoroSessionPool>(
        evb,
        FLAGS_target_host,
        FLAGS_target_port,
        poolParams,
        std::move(quicConnParams));
  } else if (FLAGS_target_tls) {
    // TLS configuration (HTTP/1.1 or HTTP/2 over TCP)
    auto connParams = HTTPCoroConnector::defaultConnectionParams();
    connParams.fizzContextAndVerifier =
        HTTPCoroConnector::makeFizzClientContextAndVerifier(
            HTTPCoroConnector::defaultTLSParams());

    return std::make_unique<HTTPCoroSessionPool>(
        evb,
        FLAGS_target_host,
        FLAGS_target_port,
        poolParams,
        std::move(connParams));
  } else {
    // Plaintext configuration
    return std::make_unique<HTTPCoroSessionPool>(
        evb, FLAGS_target_host, FLAGS_target_port, poolParams);
  }
}

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  ::gflags::ParseCommandLineFlags(&argc, &argv, false);

  XLOG(INFO) << "=== FOSS Revproxy Traffic Client ===";
  XLOG(INFO) << "Target: " << FLAGS_target_host << ":" << FLAGS_target_port;
  XLOG(INFO) << "TLS: " << (FLAGS_target_tls ? "enabled" : "disabled");
  if (FLAGS_target_tls && FLAGS_quic) {
    XLOG(INFO) << "QUIC/HTTP3: enabled";
  }
  XLOG(INFO) << "Target RPS: " << FLAGS_target_rps;
  XLOG(INFO) << "Duration: " << FLAGS_duration_sec << " seconds";
  XLOG(INFO) << "Connections: " << FLAGS_num_connections;
  XLOG(INFO) << "Streams per connection: " << FLAGS_streams_per_connection;
  XLOG(INFO) << "Reset probability: " << FLAGS_reset_probability;

  Metrics metrics;
  metrics.startTime = std::chrono::steady_clock::now();

  std::atomic<bool> shouldStop{false};
  folly::EventBase evb;
  auto pool = createConnectionPool(&evb);

  // Schedule duration timer
  evb.runAfterDelay(
      [&shouldStop, &evb]() {
        XLOG(INFO) << "Duration expired, stopping...";
        shouldStop.store(true);
        evb.terminateLoopSoon();
      },
      FLAGS_duration_sec * 1000); // milliseconds

  // Launch workers
  evb.runInEventBaseThread([poolPtr = pool.get(),
                            metricsPtr = &metrics,
                            shouldStopPtr = &shouldStop,
                            evbPtr = &evb]() {
    // Create tasks
    std::vector<folly::coro::Task<void>> tasks;
    tasks.reserve(FLAGS_num_connections);
    for (int i = 0; i < FLAGS_num_connections; ++i) {
      tasks.push_back(
          createTrafficWorkerTask(*poolPtr, *metricsPtr, *shouldStopPtr, i));
    }

    // Start all workers
    folly::coro::co_withExecutor(
        evbPtr, runAllWorkers(std::move(tasks), shouldStopPtr, evbPtr))
        .start();
  });

  // Run event loop
  XLOG(INFO) << "Starting event loop...";
  evb.loop();

  // Print final statistics
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - metrics.startTime);

  XLOG(INFO) << "=== Final Statistics ===";
  XLOG(INFO) << "Requests sent: " << metrics.requestsSent;
  XLOG(INFO) << "Responses received: " << metrics.responsesReceived;
  XLOG(INFO) << "Errors: " << metrics.errors;
  XLOG(INFO) << "Resets: " << metrics.resets;
  XLOG(INFO) << "Elapsed time: " << elapsed.count() << " ms";

  if (elapsed.count() > 0) {
    double actualRps = (metrics.requestsSent * 1000.0) / elapsed.count();
    XLOG(INFO) << "Actual RPS: " << actualRps;
  }

  // Drain pool
  pool->drain();

  XLOG(INFO) << "=== Traffic Client Shutdown ===";
  return 0;
}
