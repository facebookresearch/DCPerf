/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ti/foss_revproxy/proxy/ProxyHandler.h"

#include <folly/logging/xlog.h>

#include "proxygen/lib/http/coro/HTTPFixedSource.h"
#include "proxygen/lib/http/coro/HTTPHybridSource.h"

using namespace proxygen;
using namespace proxygen::coro;

namespace ti {
namespace foss_revproxy {

// Connection pool configuration constants
namespace {
constexpr std::chrono::seconds DEFAULT_CONNECT_TIMEOUT{5};
constexpr size_t MAX_CONNECTION_ATTEMPTS = 3;
} // namespace

ProxyHandler::ProxyHandler(
    std::vector<Backend> backends,
    std::shared_ptr<LoadBalancer> loadBalancer,
    ProxyConfig config,
    std::shared_ptr<ProxyMetrics> metrics)
    : backends_(std::move(backends)),
      loadBalancer_(std::move(loadBalancer)),
      config_(std::move(config)),
      metrics_(
          metrics ? std::move(metrics) : std::make_shared<ProxyMetrics>()) {}

HTTPCoroSessionPool& ProxyHandler::getBackendPool(
    folly::EventBase* evb,
    size_t backendIdx) {
  using PoolKey = std::pair<folly::EventBase*, size_t>;
  // TODO(sunobrien): ensure thread safety, leaving this for now since I'm going
  // to change this implementation anyways as part of the upcoming KR
  static std::map<PoolKey, std::unique_ptr<HTTPCoroSessionPool>> pools;

  auto key = std::make_pair(evb, backendIdx);
  auto it = pools.find(key);

  if (it == pools.end()) {
    // Create new pool for this (EventBase, backend) pair
    if (backendIdx >= backends_.size()) {
      throw std::out_of_range("Backend index out of range");
    }

    const auto& backend = backends_[backendIdx];
    bool useH2 = config_.backendH2;

    XLOG(INFO) << "Creating pool for backend " << backendIdx << ": "
               << backend.host << ":" << backend.port
               << (backend.tls ? " (TLS, ALPN-negotiated)"
                               : (useH2 ? " (plaintext, HTTP/2)"
                                        : " (plaintext, HTTP/1.1)"));

    std::unique_ptr<HTTPCoroSessionPool> pool;
    HTTPCoroSessionPool::PoolParams poolParams;
    poolParams.connectTimeout = DEFAULT_CONNECT_TIMEOUT;
    poolParams.maxConnectionAttempts = MAX_CONNECTION_ATTEMPTS;

    if (backend.tls) {
      // TLS configuration
      auto connParams = HTTPCoroConnector::defaultConnectionParams();
      connParams.serverName = backend.host;
      connParams.fizzContextAndVerifier =
          HTTPCoroConnector::makeFizzClientContextAndVerifier(
              HTTPCoroConnector::defaultTLSParams());

      pool = std::make_unique<HTTPCoroSessionPool>(
          evb, backend.host, backend.port, poolParams, connParams);
    } else {
      auto connParams = HTTPCoroConnector::defaultConnectionParams();
      if (useH2) {
        connParams.plaintextProtocol = "h2";
      }

      pool = std::make_unique<HTTPCoroSessionPool>(
          evb, backend.host, backend.port, poolParams, connParams);
    }

    it = pools.emplace(key, std::move(pool)).first;
  }

  return *it->second;
}

folly::coro::Task<HTTPSourceHolder> ProxyHandler::handleRequest(
    folly::EventBase* evb,
    HTTPSessionContextPtr /* ctx */,
    HTTPSourceHolder requestSource) {
  auto requestStart = std::chrono::steady_clock::now();

  metrics_->requestsReceived++;

  auto headerEvent = co_await co_awaitTry(requestSource.readHeaderEvent());
  if (headerEvent.hasException()) {
    XLOG(ERR) << "Failed to read request headers: "
              << headerEvent.exception().what();
    recordFailure(requestStart);
    co_return getDirectResponse(400, "Bad Request");
  }

  XLOG(DBG2) << "Received request: " << headerEvent->headers->getMethodString()
             << " " << headerEvent->headers->getPath();
  // Check if we should send a direct response (for testing)
  if (config_.enableDirectResponse &&
      headerEvent->headers->getPath() == "/direct") {
    XLOG(INFO) << "Sending direct response for /direct";
    recordSuccess(requestStart, requestStart);
    co_return getDirectResponse(200, "Direct response from proxy\n");
  }

  // Forward to backend
  // For requests with no body (eom=true), pass an empty source to avoid hanging
  co_return co_await forwardToBackend(
      evb,
      std::move(headerEvent->headers),
      headerEvent->eom ? HTTPSourceHolder() : std::move(requestSource),
      requestStart);
}

folly::coro::Task<HTTPSourceHolder> ProxyHandler::forwardToBackend(
    folly::EventBase* evb,
    std::unique_ptr<HTTPMessage> headers,
    HTTPSourceHolder requestSource,
    std::chrono::steady_clock::time_point requestStart) {
  // Check if we have any backends configured
  if (backends_.empty()) {
    XLOG(ERR) << "No backends configured";
    recordFailure(requestStart);
    co_return getDirectResponse(503, "No backends available\n");
  }

  // Select backend using load balancer
  auto backendIndexOpt = loadBalancer_->selectBackend(backends_.size());
  if (!backendIndexOpt.has_value()) {
    XLOG(ERR) << "Load balancer failed to select a backend";
    recordFailure(requestStart);
    co_return getDirectResponse(503, "Backend selection failed\n");
  }
  size_t backendIndex = backendIndexOpt.value();

  XLOG(DBG2) << "Selected backend " << backendIndex << " of "
             << backends_.size();

  try {
    // Get connection pool for this backend and EventBase
    // getBackendPool() lazily creates pools per (EventBase, backend) pair
    auto& pool = getBackendPool(evb, backendIndex);

    XLOG(DBG3) << "Getting connection from pool...";

    auto backendStart = std::chrono::steady_clock::now();
    auto res = co_await co_awaitTry(pool.getSessionWithReservation());

    if (res.hasException()) {
      XLOG(ERR) << "Failed to connect to backend " << backendIndex << ": "
                << res.exception().what();
      recordFailure(requestStart);
      co_return getDirectResponse(503, "Backend connection failed\n");
    }

    XLOG(DBG3) << "Got connection, forwarding request to backend "
               << backendIndex;

    // Forward the request to the backend
    // HTTPHybridSource combines headers + body stream
    auto response = co_await res->session->sendRequest(
        new HTTPHybridSource(std::move(headers), std::move(requestSource)),
        std::move(res->reservation));

    recordSuccess(requestStart, backendStart);

    co_return std::move(response);

  } catch (const std::exception& ex) {
    XLOG(ERR) << "Exception while getting connection: " << ex.what();
    recordFailure(requestStart);
    co_return getDirectResponse(503, "Backend connection exception\n");
  }
}

HTTPSourceHolder ProxyHandler::getDirectResponse(
    int statusCode,
    const std::string& body) {
  return HTTPFixedSource::makeFixedResponse(statusCode, body);
}

void ProxyHandler::recordFailure(
    std::chrono::steady_clock::time_point requestStart) {
  metrics_->requestsFailed++;
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - requestStart);
  metrics_->totalLatencyUs += elapsed.count();
}

void ProxyHandler::recordSuccess(
    std::chrono::steady_clock::time_point requestStart,
    std::chrono::steady_clock::time_point backendStart) {
  metrics_->requestsSucceeded++;
  auto backendElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - backendStart);
  metrics_->backendLatencyUs += backendElapsed.count();
  auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - requestStart);
  metrics_->totalLatencyUs += totalElapsed.count();
}

} // namespace foss_revproxy
} // namespace ti
