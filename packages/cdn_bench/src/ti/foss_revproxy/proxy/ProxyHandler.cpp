/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ti/foss_revproxy/proxy/ProxyHandler.h"

#include <folly/Synchronized.h>
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
  using PoolMap = std::map<PoolKey, std::unique_ptr<HTTPCoroSessionPool>>;

  static folly::Synchronized<PoolMap> pools;

  auto key = std::make_pair(evb, backendIdx);

  // Fast path: read lock to check if pool already exists
  {
    auto locked = pools.rlock();
    auto it = locked->find(key);
    if (it != locked->end()) {
      return *it->second;
    }
  }

  // Slow path: create pool outside the lock, then insert
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

  HTTPCoroSessionPool::PoolParams poolParams;
  poolParams.connectTimeout = DEFAULT_CONNECT_TIMEOUT;
  poolParams.maxConnectionAttempts = MAX_CONNECTION_ATTEMPTS;

  auto connParams = HTTPCoroConnector::defaultConnectionParams();
  if (backend.tls) {
    connParams.serverName = backend.host;
    connParams.fizzContextAndVerifier =
        HTTPCoroConnector::makeFizzClientContextAndVerifier(
            HTTPCoroConnector::defaultTLSParams());
  } else if (useH2) {
    connParams.plaintextProtocol = "h2";
  }

  // Create pool on the caller's EventBase thread (outside any lock)
  auto pool = std::make_unique<HTTPCoroSessionPool>(
      evb, backend.host, backend.port, poolParams, connParams);

  // Now insert under write lock
  auto locked = pools.wlock();

  // Verify after acquiring write lock (other thread may have created it)
  auto it = locked->find(key);
  if (it != locked->end()) {
    // Another thread won out discard pool and use theirs
    return *it->second;
  }

  auto [inserted, success] = locked->emplace(key, std::move(pool));
  return *inserted->second;
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

    // Read the response header event to catch transport errors early.
    // Without this, TRANSPORT_READ_ERROR leaks to the downstream session
    // and triggers noisy "Application supplied internal error code" warnings.
    auto respHeaderTry = co_await co_awaitTry(response.readHeaderEvent());
    if (respHeaderTry.hasException()) {
      XLOG(DBG2) << "Backend " << backendIndex
                 << " response error (likely connection warmup): "
                 << respHeaderTry.exception().what();
      recordFailure(requestStart);
      co_return getDirectResponse(502, "Backend response error\n");
    }

    recordSuccess(requestStart, backendStart);

    // Re-wrap the already-consumed headers + remaining body source into a
    // new HTTPHybridSource for the downstream session
    co_return HTTPSourceHolder(new HTTPHybridSource(
        std::move(respHeaderTry->headers),
        respHeaderTry->eom ? HTTPSourceHolder() : std::move(response)));

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
