/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>
#include <atomic>
#include <iomanip>
#include <memory>
#include <thread>
#include <vector>

#include "proxygen/lib/http/coro/server/HTTPServer.h"
#include "ti/foss_revproxy/proxy/LoadBalancer.h"
#include "ti/foss_revproxy/proxy/ProxyHandler.h"

// Server configuration
DEFINE_int32(port, 8081, "Port to listen on");
DEFINE_string(cert, "", "Certificate file (for TLS/QUIC)");
DEFINE_string(key, "", "Key file (for TLS/QUIC)");
DEFINE_string(plaintext_proto, "", "Plaintext protocol (h1, h2, etc.)");
DEFINE_bool(quic, false, "Enable QUIC/HTTP3 (requires cert/key)");

// Backend configuration
DEFINE_string(
    backend_servers,
    "",
    "Comma-separated list of backend server addresses (e.g., '::1,::1,::1')");
DEFINE_string(
    backend_ports,
    "",
    "Comma-separated list of backend server ports (e.g., '8082,8083,8084')");
DEFINE_bool(backend_tls, false, "Use TLS for backend connections");
DEFINE_bool(backend_h2, false, "Use HTTP/2 for plaintext backend connections");

// Load balancing
DEFINE_string(
    lb_algorithm,
    "random",
    "Load balancing algorithm: 'random' or 'roundrobin'");

// Feature flags
DEFINE_bool(
    enable_direct_response,
    false,
    "Enable direct response mode (send responses without backend)");

// Metrics configuration
DEFINE_int32(
    metrics_interval,
    5,
    "Interval in seconds between metrics output (0 = disabled)");
DEFINE_bool(metrics_summary, true, "Print final metrics summary on shutdown");

using namespace proxygen;
using namespace proxygen::coro;
using namespace ti::foss_revproxy;

namespace {

/**
 * Parse comma-separated values into a vector
 * Reports warnings for empty elements that are skipped
 */
std::vector<std::string> parseCSV(const std::string& csv) {
  std::vector<std::string> result;

  if (csv.empty()) {
    return result;
  }

  std::stringstream ss(csv);
  std::string item;
  size_t position = 0;
  bool foundEmptyElement = false;

  while (std::getline(ss, item, ',')) {
    // Trim whitespace
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);

    if (!item.empty()) {
      result.push_back(item);
    } else {
      foundEmptyElement = true;
      XLOG(WARN) << "Skipping empty element at position " << position
                 << " in CSV: '" << csv << "'";
    }
    position++;
  }

  if (foundEmptyElement) {
    XLOG(WARN) << "CSV input had empty elements that were skipped";
  }

  return result;
}

/**
 * Create load balancer based on configuration and return it
 */
std::shared_ptr<LoadBalancer> createLoadBalancer() {
  LoadBalancerStrategy strategy;
  try {
    strategy = parseLoadBalancerStrategy(FLAGS_lb_algorithm);
  } catch (const std::invalid_argument& e) {
    XLOG(WARN) << "Invalid load balancer algorithm '" << FLAGS_lb_algorithm
               << "': " << e.what() << ". Defaulting to Random.";
    strategy = LoadBalancerStrategy::RANDOM;
  }

  std::shared_ptr<LoadBalancer> loadBalancer;
  switch (strategy) {
    case LoadBalancerStrategy::ROUND_ROBIN:
      XLOG(INFO) << "Using RoundRobin load balancing";
      loadBalancer = std::make_shared<RoundRobinLoadBalancer>();
      break;
    case LoadBalancerStrategy::RANDOM:
    default: // default not needed, but makes linter and Devmate Reviewer happy
      XLOG(INFO) << "Using Random load balancing";
      loadBalancer = std::make_shared<RandomLoadBalancer>();
      break;
  }

  return loadBalancer;
}

/**
 * Configure backends from flags and return backend vector
 */
std::vector<Backend> configureBackends() {
  std::vector<Backend> backends;

  if (FLAGS_backend_servers.empty() || FLAGS_backend_ports.empty()) {
    XLOG(WARN) << "No backends configured. Proxy will return 503 for all "
                  "requests (unless direct response is enabled).";
    return backends;
  }

  auto servers = parseCSV(FLAGS_backend_servers);
  auto ports = parseCSV(FLAGS_backend_ports);

  if (servers.size() != ports.size()) {
    XLOG(ERR) << "Number of backend servers (" << servers.size()
              << ") does not match number of backend ports (" << ports.size()
              << ")";
    throw std::invalid_argument("Mismatched backend servers and ports");
  }

  XLOG(INFO) << "Configuring " << servers.size() << " backend server(s):";

  // Populate backends vector
  for (size_t i = 0; i < servers.size(); ++i) {
    // Parse and validate port number
    int portNum = std::stoi(ports[i]);
    if (portNum < 1 || portNum > 65535) {
      XLOG(ERR) << "Invalid port number: " << portNum << " (must be 1-65535)";
      throw std::invalid_argument(
          "Port must be in range 1-65535, got: " + std::to_string(portNum));
    }
    uint16_t port = static_cast<uint16_t>(portNum);
    backends.push_back({servers[i], port, FLAGS_backend_tls});
    XLOG(INFO) << "  Backend " << i << ": " << servers[i] << ":" << port
               << (FLAGS_backend_tls ? " (TLS)" : " (plaintext)");
  }

  return backends;
}

/**
 * Configure TLS if cert/key are provided
 */
void configureTLS(HTTPServer::Config& config) {
  if (!FLAGS_cert.empty()) {
    auto tlsConfig = HTTPServer::getDefaultTLSConfig();
    try {
      tlsConfig.setCertificate(FLAGS_cert, FLAGS_key, "");
    } catch (const std::exception& ex) {
      XLOG(ERR) << "Invalid certificate or key file: " << ex.what();
      throw;
    }
    config.socketConfig.sslContextConfigs.emplace_back(std::move(tlsConfig));

    if (FLAGS_quic) {
      XLOG(INFO) << "Enabling QUIC/HTTP3 support";
      config.quicConfig = HTTPServer::QuicConfig();
    }
  } else if (FLAGS_quic) {
    XLOG(ERR) << "QUIC requires --cert and --key";
    throw std::invalid_argument("QUIC requires certificates");
  }
}

} // namespace

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  ::gflags::ParseCommandLineFlags(&argc, &argv, false);

  XLOG(INFO) << "=== FOSS Revproxy Starting ===";
  XLOG(INFO) << "Listening on port " << FLAGS_port;

  if (!FLAGS_cert.empty()) {
    XLOG(INFO) << "TLS enabled with cert: " << FLAGS_cert;
    if (FLAGS_quic) {
      XLOG(INFO) << "QUIC/HTTP3 enabled";
    }
  } else {
    XLOG(INFO) << "Running in plaintext mode";
  }

  HTTPServer::Config httpServerConfig;
  // Bind to IPv6 wildcard (::) which accepts both IPv4 and IPv6 on dual-stack
  httpServerConfig.socketConfig.bindAddress.setFromIpPort("::", FLAGS_port);
  httpServerConfig.plaintextProtocol = FLAGS_plaintext_proto;

  configureTLS(httpServerConfig);

  auto backends = configureBackends();

  auto loadBalancer = createLoadBalancer();

  // Create shared metrics object
  auto metrics = std::make_shared<ProxyMetrics>();

  ProxyConfig proxyConfig{
      .enableDirectResponse = FLAGS_enable_direct_response,
      .backendH2 = FLAGS_backend_h2};

  if (FLAGS_backend_h2) {
    XLOG(INFO) << "HTTP/2 enabled for backend connections";
  }

  auto handler = std::make_shared<ProxyHandler>(
      backends, loadBalancer, proxyConfig, metrics);

  XLOG(INFO) << "Backend configuration complete";
  XLOG(INFO) << "Starting HTTP server...";

  // Start metrics reporting thread if interval > 0
  std::atomic<bool> stopMetrics{false};
  std::thread metricsThread;

  if (FLAGS_metrics_interval > 0) {
    XLOG(INFO) << "Metrics reporting enabled every " << FLAGS_metrics_interval
               << " seconds";
    metricsThread = std::thread([&metrics, &stopMetrics]() {
      while (!stopMetrics) {
        std::this_thread::sleep_for(
            std::chrono::seconds(FLAGS_metrics_interval));
        if (stopMetrics) {
          break;
        }

        // Print periodic metrics
        XLOG(INFO) << "=== Proxy Metrics ===";
        XLOG(INFO) << "Elapsed: " << std::fixed << std::setprecision(1)
                   << metrics->getElapsedSeconds() << "s";
        XLOG(INFO) << "Requests: " << metrics->requestsReceived
                   << " | Success: " << metrics->requestsSucceeded
                   << " | Failed: " << metrics->requestsFailed;
        XLOG(INFO) << "RPS: " << std::fixed << std::setprecision(1)
                   << metrics->getActualRPS()
                   << " | Success Rate: " << std::setprecision(2)
                   << metrics->getSuccessRate() << "%";
        XLOG(INFO) << "Avg Latency: " << std::setprecision(2)
                   << metrics->getAvgLatencyMs()
                   << "ms | Backend: " << metrics->getAvgBackendLatencyMs()
                   << "ms";
        if (metrics->retriesAttempted > 0) {
          XLOG(INFO) << "Retries: " << metrics->retriesAttempted
                     << " | Retries Succeeded: " << metrics->retriesSucceeded;
        }
      }
    });
  }

  HTTPServer server(std::move(httpServerConfig), handler);
  server.start();

  // Stop metrics thread
  stopMetrics = true;
  if (metricsThread.joinable()) {
    metricsThread.join();
  }

  // Print final summary
  if (FLAGS_metrics_summary) {
    XLOG(INFO) << "=== Final Proxy Statistics ===";
    XLOG(INFO) << "Total Elapsed: " << std::fixed << std::setprecision(2)
               << metrics->getElapsedSeconds() << " seconds";
    XLOG(INFO) << "Requests Received: " << metrics->requestsReceived;
    XLOG(INFO) << "Requests Succeeded: " << metrics->requestsSucceeded;
    XLOG(INFO) << "Requests Failed: " << metrics->requestsFailed;
    XLOG(INFO) << "Success Rate: " << std::setprecision(2)
               << metrics->getSuccessRate() << "%";
    XLOG(INFO) << "Actual RPS: " << std::setprecision(1)
               << metrics->getActualRPS();
    XLOG(INFO) << "Avg Total Latency: " << std::setprecision(3)
               << metrics->getAvgLatencyMs() << " ms";
    XLOG(INFO) << "Avg Backend Latency: " << metrics->getAvgBackendLatencyMs()
               << " ms";
    XLOG(INFO) << "Retries Attempted: " << metrics->retriesAttempted;
    XLOG(INFO) << "Retries Succeeded: " << metrics->retriesSucceeded;
  }

  XLOG(INFO) << "=== FOSS Revproxy Shutdown ===";

  return 0;
}
