/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchClient.h"

#include <fmt/format.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#include <folly/Random.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Promise.h>
#include <folly/coro/Timeout.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>
#include <mcrouter/McrouterFiberContext.h>
#include <mcrouter/ProxyBase.h>
#include <mcrouter/lib/carbon/Result.h>
#include <mcrouter/lib/fbi/hash.h>
#include <mcrouter/lib/network/CpuController.h>
#include <mcrouter/stats.h>
#include <set>

DECLARE_string(config);
DECLARE_uint32(warmup_ops);
DECLARE_uint32(warmup_ops_per_key);
DECLARE_uint32(benchmark_ops);
DECLARE_double(get_ratio);
DECLARE_uint32(key_size);
DECLARE_uint32(value_size);
DECLARE_uint32(num_keys);
DECLARE_uint32(num_threads);
DECLARE_bool(verbose);
DECLARE_bool(enable_zipfian);
DECLARE_double(zipfian_skew);
DECLARE_uint32(max_inflight);
DECLARE_string(traffic_distribution);

// Declare admin port flag (will be defined in main.cpp)
// Note: We use server_host for admin connection since admin server
// runs on the same machine as the cache server
DECLARE_uint32(admin_port);

// Declare server connection flags (will be defined in main.cpp)
DECLARE_string(server_host);
DECLARE_uint32(server_port);
DECLARE_uint32(duration_seconds);

namespace facebook {
namespace ucachebench {

// Default socket timeout in seconds to prevent indefinite blocking.
// 600 seconds (10 minutes) is long enough for normal multi-client
// coordination but prevents the client from hanging forever.
constexpr uint32_t kDefaultTimeoutSeconds = 600;

// ============================================================================
// AdminConnection implementation
// ============================================================================

AdminConnection::~AdminConnection() {
  disconnect();
}

bool AdminConnection::connect(const std::string& host, uint16_t port) {
  if (socket_ >= 0) {
    disconnect();
  }

  // Resolve hostname
  struct addrinfo hints, *result;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM;

  std::string portStr = std::to_string(port);
  int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
  if (ret != 0) {
    printf(
        "[AdminConnection] Failed to resolve host %s: %s\n",
        host.c_str(),
        gai_strerror(ret));
    return false;
  }

  // Try each address until we connect
  for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
    socket_ = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (socket_ < 0) {
      continue;
    }

    if (::connect(socket_, rp->ai_addr, rp->ai_addrlen) == 0) {
      freeaddrinfo(result);

      // Set default receive timeout during connection setup to prevent
      // indefinite blocking if the server disconnects or the application
      // needs to shut down. 600 seconds (10 minutes) is long enough for
      // normal multi-client coordination but prevents hanging forever.
      struct timeval tv;
      tv.tv_sec = kDefaultTimeoutSeconds;
      tv.tv_usec = 0;
      if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        printf(
            "[AdminConnection] Warning: Failed to set socket timeout: %s\n",
            strerror(errno));
      }

      printf(
          "[AdminConnection] Connected to admin server at %s:%u\n",
          host.c_str(),
          port);
      return true;
    }

    ::close(socket_);
    socket_ = -1;
  }

  freeaddrinfo(result);
  printf(
      "[AdminConnection] Failed to connect to %s:%u: %s\n",
      host.c_str(),
      port,
      strerror(errno));
  return false;
}

void AdminConnection::disconnect() {
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
  readBuffer_.clear();
}

std::string AdminConnection::sendCommand(const std::string& command) {
  if (socket_ < 0) {
    return "ERROR Not connected";
  }

  // Send command
  std::string msg = command + "\n";
  ssize_t sent = send(socket_, msg.c_str(), msg.size(), 0);
  if (sent < 0) {
    printf("[AdminConnection] Send failed: %s\n", strerror(errno));
    return "ERROR Send failed";
  }

  // Read response, filtering out any broadcast notifications
  while (true) {
    std::string line = readLine();
    if (line.empty()) {
      return "ERROR Read failed";
    }

    // Check if this is a broadcast notification
    if (isBroadcastNotification(line)) {
      // Buffer it for later retrieval via waitForNotification()
      pendingNotifications_.push_back(line);
      continue;
    }

    // This is the actual response to our command
    return line;
  }
}

bool AdminConnection::isBroadcastNotification(const std::string& message) {
  // Broadcast notifications from the server are:
  // - ALL_REGISTERED
  // - ALL_WARMUP_DONE
  // - ALL_DONE
  // Command responses start with "OK" or "ERROR" or "STATUS"
  return message == "ALL_REGISTERED" || message == "ALL_WARMUP_DONE" ||
      message == "ALL_DONE";
}

std::string AdminConnection::readLine() {
  // Check if we already have a complete line in the buffer
  size_t pos = readBuffer_.find('\n');
  if (pos != std::string::npos) {
    std::string line = readBuffer_.substr(0, pos);
    readBuffer_.erase(0, pos + 1);
    // Remove trailing \r if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  // Read more data
  char buffer[1024];
  while (true) {
    ssize_t bytesRead = recv(socket_, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
      if (bytesRead == 0) {
        printf("[AdminConnection] Connection closed by server\n");
      } else {
        printf("[AdminConnection] Recv failed: %s\n", strerror(errno));
      }
      return "";
    }

    buffer[bytesRead] = '\0';
    readBuffer_ += buffer;

    pos = readBuffer_.find('\n');
    if (pos != std::string::npos) {
      std::string line = readBuffer_.substr(0, pos);
      readBuffer_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
  }
}

int32_t AdminConnection::sendRegister() {
  std::string response = sendCommand("REGISTER");
  if (response.empty()) {
    return -1;
  }

  // Parse "OK <client_id>"
  if (response.substr(0, 3) == "OK ") {
    try {
      return std::stoi(response.substr(3));
    } catch (const std::exception&) {
      printf(
          "[AdminConnection] Failed to parse client ID from: %s\n",
          response.c_str());
      return -1;
    }
  }

  printf("[AdminConnection] REGISTER failed: %s\n", response.c_str());
  return -1;
}

bool AdminConnection::sendWarmupDone(int32_t clientId) {
  std::string response = sendCommand("WARMUP_DONE " + std::to_string(clientId));
  return response == "OK";
}

bool AdminConnection::sendBenchmarkDone(int32_t clientId) {
  std::string response =
      sendCommand("BENCHMARK_DONE " + std::to_string(clientId));
  return response == "OK";
}

std::string AdminConnection::waitForNotification(uint32_t timeoutSeconds) {
  if (socket_ < 0) {
    return "";
  }

  // First check if we have any buffered notifications from sendCommand()
  if (!pendingNotifications_.empty()) {
    std::string notification = pendingNotifications_.front();
    pendingNotifications_.erase(pendingNotifications_.begin());
    return notification;
  }

  // If a custom timeout is specified, set it temporarily.
  // Otherwise, use the default timeout set during connection setup.
  bool customTimeout = (timeoutSeconds > 0);
  if (customTimeout) {
    struct timeval tv;
    tv.tv_sec = timeoutSeconds;
    tv.tv_usec = 0;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      printf(
          "[AdminConnection] Failed to set socket timeout: %s\n",
          strerror(errno));
    }
  }

  std::string line = readLine();

  // Reset to default timeout if a custom one was used
  if (customTimeout) {
    struct timeval tv;
    tv.tv_sec = kDefaultTimeoutSeconds;
    tv.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  return line;
}

} // namespace ucachebench
} // namespace facebook

// Server connection flags
DEFINE_string(server_host, "::1", "Server hostname or IP address");
DEFINE_uint32(server_port, 11211, "Server port");
DEFINE_uint32(duration_seconds, 60, "Benchmark duration in seconds");

DEFINE_uint32(warmup_seconds, 10, "Warmup duration in seconds");
DEFINE_uint32(
    progress_interval_seconds,
    5,
    "Progress reporting interval in seconds (0 = disable)");
DEFINE_uint32(key_count, 100000, "Number of unique keys");
DEFINE_uint32(value_size_min, 64, "Minimum value size in bytes");
DEFINE_uint32(value_size_max, 1024, "Maximum value size in bytes");
DEFINE_double(get_ratio, 0.9, "Ratio of GET operations (vs SET operations)");
DEFINE_uint32(
    connection_timeout_ms,
    5000,
    "Connection timeout in milliseconds");
DEFINE_uint32(send_timeout_ms, 5000, "Send timeout in milliseconds");
DEFINE_string(
    security_mech,
    "plain",
    "Security mechanism for mcrouter (plain, tls_to_plain, fizz, etc.)");
DEFINE_uint32(
    num_proxies,
    0,
    "Number of mcrouter proxy threads (0 = auto-detect using hardware_concurrency). "
    "Ignored when --num_connections is set.");
DEFINE_uint32(
    max_inflight,
    1,
    "Maximum number of concurrent in-flight requests (higher = better throughput, requires more memory). "
    "Ignored when --auto_concurrency is enabled.");
DEFINE_uint32(
    num_threads,
    0,
    "Number of client worker threads for request generation (0 = auto-detect, recommended: 4-16)");
DEFINE_uint32(
    additional_fanout,
    0,
    "Number of additional connections per server for fanout (0 = disabled). "
    "Limited by ephemeral port range per source IP (~64K). Use LD_PRELOAD=bind_source.so "
    "with multiple source IPs for higher connection counts. "
    "Ignored when --num_connections is set.");
DEFINE_uint32(
    num_connections,
    0,
    "Target number of TCP connections to the server (0 = disabled, use num_proxies/additional_fanout directly). "
    "When set, automatically derives num_proxies and additional_fanout. "
    "Enforces the mcrouter limit of 32768 ProxyDestinations per instance. "
    "Examples: 16000 for 16K connections, 64000 for 64K connections.");
DEFINE_bool(
    auto_concurrency,
    false,
    "Automatically discover optimal concurrency during the benchmark phase using AIMD. "
    "Splits the benchmark into a ramp phase (finds peak throughput) and a steady phase "
    "(holds optimal concurrency, collects measurements). When enabled, --max_inflight "
    "is treated as the upper bound, and the controller finds the best value below it.");
DEFINE_uint32(
    ramp_seconds,
    30,
    "Duration of the AIMD ramp phase when --auto_concurrency is enabled. "
    "The controller searches for optimal concurrency during this period.");
DEFINE_double(
    target_utilization,
    1.0,
    "Target fraction of peak concurrency to use during steady state (0.0-1.0). "
    "1.0 = use peak concurrency found by AIMD. 0.9 = back off to 90%% of peak. "
    "Only used when --auto_concurrency is enabled.");
DEFINE_bool(
    enable_random_source_ip,
    false,
    "Enable random source IP addresses for connection fanout (works with BucketHashSelector)");
DEFINE_uint32(
    connection_ramp_seconds,
    10,
    "Seconds to gradually ramp up connections before warmup. "
    "Prevents connection storm when additional_fanout is large. "
    "0 = disabled (all connections established at once)");
DEFINE_uint32(
    warmup_max_inflight,
    0,
    "Max inflight per thread during warmup (0 = use --max_inflight). "
    "Set higher than --max_inflight to populate the cache faster during warmup "
    "while using low inflight during measurement for realistic packet patterns.");
DEFINE_bool(
    warmup_adaptive_load,
    true,
    "Enable adaptive load control during warmup (TCP congestion control style). "
    "Starts with low concurrency and ramps up until errors spike, then backs off. "
    "Prevents TKO cascade when multiple clients warm up simultaneously");
DEFINE_uint32(
    warmup_initial_inflight,
    2,
    "Initial max inflight per thread when adaptive load is enabled. "
    "Total initial concurrency = num_threads * warmup_initial_inflight");
DEFINE_uint32(
    failures_until_tko,
    0,
    "Number of consecutive failures before mcrouter marks a server as TKO "
    "(0 = use mcrouter default of 3). Production typically uses 12-32. "
    "With many proxy threads, a low value causes premature TKO because the "
    "counter is global across all threads");
DEFINE_bool(verbose, false, "Enable verbose logging");
DEFINE_bool(
    use_same_thread_client,
    true,
    "Use createSameThreadClient() to eliminate cross-thread message queue hops. "
    "Workers run directly on McRouter proxy EventBases instead of a separate "
    "thread pool. This matches production's same-thread dispatch pattern and "
    "can significantly improve throughput by eliminating 2 cross-thread hops "
    "per request. Set to false for legacy remote-thread mode.");
DEFINE_bool(
    use_distribution,
    false,
    "Use production traffic distribution for key/value sizes");
DEFINE_string(
    distribution_config,
    "",
    "Path to JSON file with traffic distribution config (generated by parse_traffic_distribution.py)");

// Zipfian distribution flags for realistic hot-key access patterns
DEFINE_bool(
    zipfian,
    false,
    "Enable Zipfian key distribution to simulate production hot-key access patterns");
DEFINE_double(
    zipfian_skew,
    0.99,
    "Zipfian skew parameter (0.99 = standard Zipf, higher = more skewed). "
    "With skew=0.99, ~20%% of keys receive ~80%% of accesses");
DEFINE_double(
    hot_key_ratio,
    0.0,
    "Fraction of keys that are 'hot' (0.0 = disabled, use pure Zipfian). "
    "When set, hot_key_ratio of keys receive hot_key_frequency more accesses");

DEFINE_uint64(
    lane_phase_stagger_us,
    0,
    "Spread each measurement lane's first request over this many microseconds "
    "(0 = off). Lanes are completion-driven, so if they all start together they "
    "can remain phase-locked for the whole run and deliver load as synchronized "
    "bursts. Set to roughly one response time to decorrelate them.");
namespace {
// Latency-stage accumulators. The benchmark reports only aggregate QPS, so a
// multi-millisecond average RPC latency against a server measured idle in
// epoll_wait had no attribution at all. These split it into synchronous send,
// transport plus server, and the time a completion waits for its EventBase to
// run the continuation.
constexpr uint64_t kLatSampleEvery = 10000;

// Per-thread, not a shared atomic: a process-wide read-modify-write once per
// request would put every lane on the same cache line, which is exactly the
// kind of overhead this instrumentation exists to avoid adding.
thread_local uint64_t tLatSampleCounter = 0;

std::atomic<uint64_t> gLatSamples{0}; // samples that reached accumulation
std::atomic<uint64_t> gLatCbSamples{0}; // subset that also got a callback stamp
std::atomic<uint64_t> gLatLostSamples{0}; // claimed a slot then timed out
std::atomic<int64_t> gLatSendNs{0};
std::atomic<int64_t> gLatTransportNs{0};
std::atomic<int64_t> gLatResumeNs{0};
std::atomic<int64_t> gLatTotalNs{0};

void reportLatencyBreakdown() {
  uint64_t n = gLatSamples.load();
  uint64_t cbN = gLatCbSamples.load();
  uint64_t lost = gLatLostSamples.load();
  if (n == 0) {
    printf("LATENCY_BREAKDOWN: no samples (lost=%lu)\n", lost);
    fflush(stdout);
    return;
  }
  auto us = [](std::atomic<int64_t>& a, uint64_t d) {
    return d == 0 ? 0.0 : static_cast<double>(a.load()) / d / 1000.0;
  };
  // transport and resume are only accumulated for samples that carried a
  // callback timestamp, so they divide by cbN, not n.
  printf(
      "LATENCY_BREAKDOWN samples=%lu cb_samples=%lu lost=%lu total_us=%.1f "
      "send_us=%.1f transport_us=%.1f resume_lag_us=%.1f\n",
      n,
      cbN,
      lost,
      us(gLatTotalNs, n),
      us(gLatSendNs, n),
      us(gLatTransportNs, cbN),
      us(gLatResumeNs, cbN));
  fflush(stdout);
}

} // namespace

namespace facebook {
namespace ucachebench {

// ============================================================================
// ZipfianGenerator Implementation
// Based on YCSB's ScrambledZipfianGenerator algorithm
// Generates keys following Zipf's law: P(k) ∝ 1/k^s
// With s=0.99, approximately 20% of keys receive 80% of accesses
// ============================================================================

double ZipfianGenerator::zeta(uint64_t n, double theta) {
  double sum = 0.0;
  for (uint64_t i = 1; i <= n; i++) {
    sum += 1.0 / std::pow(static_cast<double>(i), theta);
  }
  return sum;
}

ZipfianGenerator::ZipfianGenerator(uint64_t numItems, double skew)
    : numItems_(numItems), skew_(skew), theta_(skew) {
  // Precompute constants for fast sampling
  // zeta(n) = sum_{i=1}^{n} 1/i^theta
  zetan_ = zeta(numItems_, theta_);
  zetaTwo_ = zeta(2, theta_);

  // alpha = 1 / (1 - theta)
  alpha_ = 1.0 / (1.0 - theta_);

  // eta = (1 - pow(2.0/n, 1-theta)) / (1 - zetaTwo_/zetan_)
  eta_ = (1.0 - std::pow(2.0 / static_cast<double>(numItems_), 1.0 - theta_)) /
      (1.0 - zetaTwo_ / zetan_);
}

uint64_t ZipfianGenerator::next() {
  // Generate uniform random in [0, 1)
  double u = folly::Random::randDouble01();

  // Map to Zipfian distribution using inverse CDF approximation
  double uz = u * zetan_;

  if (uz < 1.0) {
    return 0;
  }

  if (uz < 1.0 + std::pow(0.5, theta_)) {
    return 1;
  }

  // Use approximation for larger values
  uint64_t ret = static_cast<uint64_t>(
      static_cast<double>(numItems_) * std::pow(eta_ * u - eta_ + 1.0, alpha_));

  // Ensure result is in valid range
  return std::min(ret, numItems_ - 1);
}

// ============================================================================
// UcacheBenchClient implementation
// ============================================================================

bool UcacheBenchClient::connectToAdmin(const std::string& host, uint16_t port) {
  adminConnection_ = std::make_unique<AdminConnection>();
  if (!adminConnection_->connect(host, port)) {
    adminConnection_.reset();
    return false;
  }

  // Register with the admin server to get our client ID
  clientId_ = adminConnection_->sendRegister();
  if (clientId_ < 0) {
    printf("[Client] Failed to register with admin server\n");
    adminConnection_->disconnect();
    adminConnection_.reset();
    return false;
  }

  printf("[Client] Registered with admin server as client %d\n", clientId_);

  // Wait for ALL_REGISTERED notification
  printf("[Client] Waiting for all clients to register...\n");
  std::string notification = adminConnection_->waitForNotification(0);
  if (notification != "ALL_REGISTERED") {
    printf(
        "[Client] Unexpected notification while waiting for ALL_REGISTERED: %s\n",
        notification.c_str());
    return false;
  }

  printf("[Client] All clients registered, ready to start warmup\n");
  return true;
}

UcacheBenchClient::UcacheBenchClient() {
  // Load traffic distribution if configured
  if (FLAGS_use_distribution) {
    if (FLAGS_distribution_config.empty()) {
      throw std::runtime_error(
          "Must specify --distribution_config when --use_distribution=true");
    }
    loadTrafficDistribution(FLAGS_distribution_config);
    distribution_.enabled = true;

    if (FLAGS_verbose) {
      printf(
          "Loaded production traffic distribution from: %s\n",
          FLAGS_distribution_config.c_str());
      printf("  GET ratio: %.4f\n", distribution_.getRatio);
      printf("  GET key size (avg): %.2f bytes\n", distribution_.getKeySizeAvg);
      printf(
          "  SET value size (avg): %.2f bytes\n",
          distribution_.setValueSizeAvg);
      fflush(stdout);
    }
  }

  // Create mcrouter options for Thrift transport
  facebook::memcache::McrouterOptions options;

  // Derive num_proxies and additional_fanout from --num_connections if set
  uint32_t effectiveNumProxies = FLAGS_num_proxies;
  uint32_t effectiveAdditionalFanout = FLAGS_additional_fanout;

  if (FLAGS_num_connections > 0) {
    constexpr uint32_t kMaxProxyDestinations = 32768;
    uint32_t hwConcurrency = std::thread::hardware_concurrency();
    uint32_t numProxies = std::min(FLAGS_num_connections, hwConcurrency);
    numProxies = std::max(1u, numProxies);

    uint32_t fanoutPerProxy =
        (FLAGS_num_connections + numProxies - 1) / numProxies;
    uint32_t additionalFanout = fanoutPerProxy > 0 ? fanoutPerProxy - 1 : 0;

    if (numProxies * (additionalFanout + 1) > kMaxProxyDestinations) {
      numProxies = std::min(numProxies, kMaxProxyDestinations);
      fanoutPerProxy = kMaxProxyDestinations / numProxies;
      additionalFanout = fanoutPerProxy > 0 ? fanoutPerProxy - 1 : 0;
      uint32_t actualConnections = numProxies * (additionalFanout + 1);
      printf(
          "[num_connections] Clamped to %u connections (mcrouter limit: %u ProxyDestinations). "
          "Use multiple client instances for higher counts.\n",
          actualConnections,
          kMaxProxyDestinations);
    }

    effectiveNumProxies = numProxies;
    effectiveAdditionalFanout = additionalFanout;

    uint32_t actualConnections =
        effectiveNumProxies * (effectiveAdditionalFanout + 1);
    printf(
        "[num_connections] %u requested -> num_proxies=%u, additional_fanout=%u, actual=%u\n",
        FLAGS_num_connections,
        effectiveNumProxies,
        effectiveAdditionalFanout,
        actualConnections);
    fflush(stdout);
  }

  // Configure for Thrift transport to ucache server
  // Build pool config with optional additional_fanout for high connection count
  std::string poolConfig;
  if (effectiveAdditionalFanout > 0) {
    poolConfig = fmt::format(
        R"json({{
    "pools": {{
      "ucache_pool": {{
        "servers": [ "{}:{}" ],
        "protocol": "thrift",
        "security_mech": "{}",
        "connect_timeout_ms": {},
        "server_timeout_ms": {},
        "additional_fanout": {}
      }}
    }},
    "route": {{
      "type": "PoolRoute",
      "pool": "ucache_pool"
    }}
  }})json",
        FLAGS_server_host,
        FLAGS_server_port,
        FLAGS_security_mech,
        FLAGS_connection_timeout_ms,
        FLAGS_send_timeout_ms,
        effectiveAdditionalFanout);
  } else {
    poolConfig = fmt::format(
        R"json({{
    "pools": {{
      "ucache_pool": {{
        "servers": [ "{}:{}" ],
        "protocol": "thrift",
        "security_mech": "{}",
        "connect_timeout_ms": {},
        "server_timeout_ms": {}
      }}
    }},
    "route": {{
      "type": "PoolRoute",
      "pool": "ucache_pool"
    }}
  }})json",
        FLAGS_server_host,
        FLAGS_server_port,
        FLAGS_security_mech,
        FLAGS_connection_timeout_ms,
        FLAGS_send_timeout_ms);
  }
  options.config_str = poolConfig;

  // Set num_proxies to use multiple threads for sending traffic
  if (effectiveNumProxies == 0) {
    options.num_proxies = std::thread::hardware_concurrency();
  } else {
    options.num_proxies = effectiveNumProxies;
  }

  // Configure TKO behavior to match production settings.
  // The default failures_until_tko=3 is too aggressive with many proxy threads
  // because the failure counter is global — just 3 timeouts from ANY thread
  // marks the server as TKO, stopping all traffic.
  if (FLAGS_failures_until_tko > 0) {
    options.failures_until_tko = FLAGS_failures_until_tko;
  }

  // Disable idle connection pruning so fanout connections stay open.
  options.reset_inactive_connection_interval = 0;

  // Set source IP for local address binding (IP aliasing).
  // When set, all outbound connections bind to this address to bypass
  // Create CarbonRouterInstance with UcacheBench RouterInfo
  auto routerPtr = facebook::memcache::mcrouter::CarbonRouterInstance<
      UcacheBenchRouterInfo>::init("ucache_bench_client", options);

  if (!routerPtr) {
    throw std::runtime_error("Failed to create mcrouter instance");
  }

  // Share ownership with the router instance
  routerInstance_ =
      std::shared_ptr<facebook::memcache::mcrouter::CarbonRouterInstance<
          UcacheBenchRouterInfo>>(
          routerPtr,
          [](facebook::memcache::mcrouter::CarbonRouterInstance<
              UcacheBenchRouterInfo>*) {
            // Custom deleter - don't delete, as the instance is managed by
            // static registry
          });

  if (FLAGS_verbose) {
    printf(
        "Connected to UcacheBench server via mcrouter at %s:%u (Thrift transport)\n",
        FLAGS_server_host.c_str(),
        FLAGS_server_port);
    printf("  McRouter proxy threads: %zu\n", options.num_proxies);
    printf("  Using per-thread clients for maximum QPS performance\n");
    if (effectiveAdditionalFanout > 0) {
      uint32_t totalConnections =
          options.num_proxies * (effectiveAdditionalFanout + 1);
      printf(
          "  Connection fanout enabled: additional_fanout=%u (total connections: %u)\n",
          effectiveAdditionalFanout,
          totalConnections);
    } else {
      printf("  Connection fanout disabled (using default connections)\n");
    }
    if (FLAGS_failures_until_tko > 0) {
      printf(
          "  TKO threshold: %u failures (overriding default of 3)\n",
          FLAGS_failures_until_tko);
    }
    if (FLAGS_enable_random_source_ip) {
      printf(
          "  Random source IP enabled: requests will use random source IPs for additional fanout\n");
    }
    fflush(stdout);
  }

  effectiveNumProxies_ = options.num_proxies;
  effectiveAdditionalFanout_ = effectiveAdditionalFanout;
}

UcacheBenchClient::~UcacheBenchClient() {
  // Clean up mcrouter instance
  if (routerInstance_) {
    routerInstance_->shutdown();
  }
}

UcacheBenchClient::WarmupResults UcacheBenchClient::warmup() {
  if (FLAGS_warmup_seconds == 0) {
    if (FLAGS_verbose) {
      printf("Warmup disabled (warmup_seconds=0)\n");
      fflush(stdout);
    }
    WarmupResults warmupResults;
    warmupResults.startTime = std::chrono::steady_clock::now();
    warmupResults.endTime = warmupResults.startTime;
    warmupResults.success = true;
    return warmupResults;
  }

  // Determine number of worker threads
  uint32_t numThreads = FLAGS_num_threads;
  if (numThreads == 0) {
    numThreads = std::max(1u, std::thread::hardware_concurrency() / 2);
  }

  uint32_t maxInflight = FLAGS_warmup_max_inflight > 0
      ? FLAGS_warmup_max_inflight
      : FLAGS_max_inflight;
  if (maxInflight < 1) {
    maxInflight = 1;
  }

  if (FLAGS_verbose) {
    printf(
        "Starting warmup for %u seconds with %u worker threads, max_inflight=%u per thread\n",
        FLAGS_warmup_seconds,
        numThreads,
        maxInflight);
    fflush(stdout);
  }

  // Connection ramp-up phase: gradually establish connections to avoid
  // overwhelming the server's accept queue when additional_fanout is large.
  // With num_proxies=64 and additional_fanout=500, mcrouter creates 32K
  // ProxyDestination objects. Without ramp-up, all connections are established
  // simultaneously on first request, causing a connection storm that TKOs the
  // server.

  auto startTime = std::chrono::steady_clock::now();
  auto endTime = startTime + std::chrono::seconds(FLAGS_warmup_seconds);

  // Thread-safe counters
  std::atomic<uint64_t> totalOps{0};
  std::atomic<uint64_t> setSuccesses{0};
  std::atomic<uint64_t> setErrors{0};
  std::atomic<bool> shouldStop{false};

  // Adaptive load control: shared dynamic inflight limit
  // All workers read this to cap their concurrency. The control thread adjusts
  // it based on observed error rate (TCP congestion control style: AIMD).
  std::atomic<uint32_t> currentMaxInflight{maxInflight};
  if (FLAGS_warmup_adaptive_load) {
    currentMaxInflight.store(
        std::min(maxInflight, FLAGS_warmup_initial_inflight));
  }

  // Progress monitoring thread
  std::thread progressThread;
  if (FLAGS_verbose && FLAGS_progress_interval_seconds > 0) {
    progressThread = std::thread([&]() {
      while (!shouldStop.load() && std::chrono::steady_clock::now() < endTime) {
        std::this_thread::sleep_for(
            std::chrono::seconds(FLAGS_progress_interval_seconds));
        if (shouldStop.load()) {
          break;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - startTime).count();
        uint64_t ops = totalOps.load();
        uint64_t successes = setSuccesses.load();
        uint64_t errors = setErrors.load();
        double avgQps = elapsed > 0 ? ops / elapsed : 0;
        double successRate = ops > 0 ? (successes * 100.0 / ops) : 0;

        printf(
            "Warmup progress: %.1fs elapsed, %lu ops (%.1f QPS avg), Success: %.1f%%, Errors: %lu, MaxInflight: %u\n",
            elapsed,
            ops,
            avgQps,
            successRate,
            errors,
            currentMaxInflight.load());
        fflush(stdout);
      }
    });
  }

  // Adaptive load control thread: monitors error rate and adjusts
  // currentMaxInflight using AIMD (Additive Increase, Multiplicative Decrease).
  // Like TCP congestion control: ramp up when healthy, cut back on errors.
  std::thread adaptiveThread;
  if (FLAGS_warmup_adaptive_load) {
    adaptiveThread = std::thread([&]() {
      constexpr auto kCheckInterval = std::chrono::seconds(2);
      constexpr double kErrorThreshold = 0.05; // 5% error rate triggers backoff
      constexpr double kHealthyThreshold = 0.01; // <1% errors = healthy, grow

      uint64_t prevOps = 0;
      uint64_t prevErrors = 0;
      bool reachedMax = false;

      while (!shouldStop.load() && std::chrono::steady_clock::now() < endTime) {
        std::this_thread::sleep_for(kCheckInterval);
        if (shouldStop.load()) {
          break;
        }

        uint64_t curOps = totalOps.load();
        uint64_t curErrors = setErrors.load();

        uint64_t windowOps = curOps - prevOps;
        uint64_t windowErrors = curErrors - prevErrors;
        prevOps = curOps;
        prevErrors = curErrors;

        if (windowOps < 100) {
          // Not enough data yet, keep growing slowly
          uint32_t cur = currentMaxInflight.load();
          if (cur < maxInflight) {
            uint32_t next = std::min(maxInflight, cur + 1);
            currentMaxInflight.store(next);
            if (FLAGS_verbose) {
              printf(
                  "Adaptive load: too few ops (%lu), inflight %u -> %u\n",
                  windowOps,
                  cur,
                  next);
              fflush(stdout);
            }
          }
          continue;
        }

        double errorRate =
            static_cast<double>(windowErrors) / static_cast<double>(windowOps);
        uint32_t cur = currentMaxInflight.load();

        if (errorRate > kErrorThreshold) {
          // Multiplicative decrease: halve the inflight limit
          uint32_t next = std::max(1u, cur / 2);
          currentMaxInflight.store(next);
          reachedMax = false;
          if (FLAGS_verbose) {
            printf(
                "Adaptive load: HIGH errors (%.1f%% of %lu ops), inflight %u -> %u (backoff)\n",
                errorRate * 100.0,
                windowOps,
                cur,
                next);
            fflush(stdout);
          }
        } else if (errorRate < kHealthyThreshold && cur < maxInflight) {
          // Additive increase: grow inflight
          // Use slow start (double) when far from max, linear when close
          uint32_t next;
          if (cur < maxInflight / 4) {
            // Slow start phase: double
            next = std::min(maxInflight, cur * 2);
          } else {
            // Congestion avoidance: linear increase
            uint32_t increment = std::max(1u, maxInflight / 10);
            next = std::min(maxInflight, cur + increment);
          }
          currentMaxInflight.store(next);
          if (next == maxInflight && !reachedMax) {
            reachedMax = true;
            if (FLAGS_verbose) {
              printf("Adaptive load: reached max inflight %u\n", maxInflight);
              fflush(stdout);
            }
          } else if (FLAGS_verbose && next != cur) {
            printf(
                "Adaptive load: healthy (%.2f%% errors), inflight %u -> %u\n",
                errorRate * 100.0,
                cur,
                next);
            fflush(stdout);
          }
        }
        // If error rate is between thresholds, hold steady
      }
    });
  }

  // Create IO thread pool for coroutine execution - matches production pattern
  folly::IOThreadPoolExecutor workerPool(numThreads);
  auto workerEvbs = workerPool.getAllEventBases();

  // Create clients for each worker with maxInflight - matches production
  // pattern McRouter's maximumOutstanding will handle backpressure via internal
  // semaphore
  std::vector<
      memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>::Pointer>
      clients;
  clients.reserve(numThreads);
  for (uint32_t i = 0; i < numThreads; ++i) {
    // Use maxInflight as maximumOutstanding - McRouter will block if limit
    // reached
    auto client = routerInstance_->createClient(maxInflight);
    if (client) {
      clients.push_back(std::move(client));
    }
  }

  // Connection activation: for each proxy, send a burst of concurrent
  // requests to force McRouter to open TCP connections to all fanout
  // destinations. Each CarbonRouterClient is pinned to one proxy, so we
  // create one client per proxy to ensure full coverage.
  if (effectiveAdditionalFanout_ > 0) {
    uint32_t scanOutstanding = effectiveAdditionalFanout_ + 1;
    uint32_t totalConns = effectiveNumProxies_ * scanOutstanding;

    if (FLAGS_verbose) {
      printf(
          "Connection activation: scanning %u proxies x %u destinations = %u total\n",
          effectiveNumProxies_,
          scanOutstanding,
          totalConns);
      fflush(stdout);
    }

    std::atomic<uint64_t> scanOps{0};

    // Create one scan client per proxy. Each createClient() call assigns
    // the client to the next proxy via round-robin (nextProxyIndex()).
    std::vector<
        memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>::Pointer>
        scanClients;
    for (uint32_t p = 0; p < effectiveNumProxies_; ++p) {
      scanClients.push_back(routerInstance_->createClient(scanOutstanding));
    }

    // For each proxy's client, launch scanOutstanding concurrent coroutines.
    // Each coroutine sends enough requests (with random keys) to cover all
    // destination slots via the coupon collector effect.
    // Need ~D*ln(D) ≈ 4000 requests for D=625 destinations to get 99% coverage.
    uint32_t requestsPerCoroutine = 100;
    auto scanWorker =
        [&](memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>*
                clientPtr,
            size_t) -> folly::coro::Task<void> {
      for (uint32_t i = 0; i < requestsPerCoroutine; ++i) {
        std::string key = generateKey();
        std::string value = generateValue();

        UcbSetRequest request;
        request.key_ref() = carbon::Keys<folly::IOBuf>(
            std::move(*folly::IOBuf::copyBuffer(key)));
        request.value_ref() = *folly::IOBuf::copyBuffer(value);
        request.exptime_ref() = 3600;

        auto [promise, future] = folly::makePromiseContract<UcbSetReply>();

        clientPtr->send(
            request,
            [p = std::move(promise)](
                const UcbSetRequest&, UcbSetReply&& reply) mutable {
              p.setValue(std::move(reply));
            });

        try {
          co_await std::move(future).within(std::chrono::seconds(10));
          scanOps++;
        } catch (const std::exception&) {
          // Request timed out or failed (e.g. send() returned false and the
          // callback never fired). Skip it rather than hang the scan join.
        }
      }
      co_return;
    };

    // Launch scan coroutines across all proxies. When connection_ramp_seconds
    // > 0, stagger the per-proxy launches so connections open GRADUALLY rather
    // than all at once. Each proxy opens ~scanOutstanding connections per
    // batch; spreading the batches over the ramp window keeps each burst under
    // the server's accept queue (somaxconn/backlog), so connections come up
    // healthy and able to serve traffic — mirroring how production accumulates
    // connections over time instead of in a storm. Without this, all
    // connections are forced open simultaneously, overwhelming the accept queue
    // and leaving many connections half-open/unable to carry requests.
    folly::coro::AsyncScope scanScope;
    const uint32_t rampMs = FLAGS_connection_ramp_seconds * 1000;
    const uint32_t perProxyDelayMs = (rampMs > 0 && effectiveNumProxies_ > 1)
        ? rampMs / effectiveNumProxies_
        : 0;
    if (FLAGS_verbose && perProxyDelayMs > 0) {
      printf(
          "Connection ramp: staggering %u proxy batches over %us (%ums/proxy)\n",
          effectiveNumProxies_,
          FLAGS_connection_ramp_seconds,
          perProxyDelayMs);
      fflush(stdout);
    }
    for (uint32_t p = 0; p < effectiveNumProxies_; ++p) {
      auto* clientPtr = scanClients[p].get();
      for (uint32_t i = 0; i < scanOutstanding; ++i) {
        scanScope.add(
            folly::coro::co_withExecutor(
                workerEvbs.at(p % workerEvbs.size()),
                scanWorker(clientPtr, i)));
      }
      // Gradually ramp: pause between proxy batches so connections establish
      // over time instead of storming the accept queue all at once. This runs
      // on the launching (non-EventBase) thread under blockingWait, so a real
      // sleep is intended here, not a coroutine yield.
      if (perProxyDelayMs > 0 && p + 1 < effectiveNumProxies_) {
        // @lint-ignore CLANGTIDY facebook-hte-BadCall-sleep_for
        std::this_thread::sleep_for(std::chrono::milliseconds(perProxyDelayMs));
      }
    }

    folly::coro::blockingWait(
        scanScope.joinAsync().scheduleOn(workerEvbs.front()));

    printf(
        "Connection activation complete: %lu requests sent across %u proxies "
        "(expected %u per proxy x %u rounds = %u total)\n",
        scanOps.load(),
        effectiveNumProxies_,
        scanOutstanding,
        requestsPerCoroutine,
        effectiveNumProxies_ * scanOutstanding * requestsPerCoroutine);
    fflush(stdout);

    // Write scan count and router diagnostics for remote debugging
    FILE* f = fopen("/tmp/ucache_scan_debug.txt", "w");
    if (f) {
      fprintf(
          f,
          "scanOps=%lu expected=%u proxies=%u coroutines=%u rounds=%u\n",
          scanOps.load(),
          effectiveNumProxies_ * scanOutstanding * requestsPerCoroutine,
          effectiveNumProxies_,
          scanOutstanding,
          requestsPerCoroutine);
      auto& proxies = routerInstance_->getProxies();
      fprintf(f, "router_proxies=%zu\n", proxies.size());

      // Test furc_hash distribution with our exact key format
      fprintf(f, "--- furc_hash distribution test ---\n");
      std::set<uint32_t> bucketsHit;
      for (uint32_t i = 0; i < 62500; i++) {
        std::string key = generateKey();
        uint32_t h = furc_hash(key.c_str(), key.size(), scanOutstanding);
        bucketsHit.insert(h);
      }
      fprintf(
          f,
          "furc_hash: 62500 keys -> %zu/%u buckets (%.1f%%)\n",
          bucketsHit.size(),
          scanOutstanding,
          100.0 * bucketsHit.size() / scanOutstanding);

      // Run ss to check connection count right after scan
      fprintf(f, "--- ss output after scan ---\n");
      fflush(f);
      system("ss -s 2>/dev/null | grep estab >> /tmp/ucache_scan_debug.txt");
      fclose(f);
    }
  }

  // Worker coroutine - matches production LoadgenWorker::co_run() pattern
  auto warmupWorker =
      [&](memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>*
              clientPtr) -> folly::coro::Task<void> {
    // CancellableAsyncScope so stragglers at warmup-end are cancelled, not
    // waited on forever (same hang as the measurement worker — see below).
    folly::coro::CancellableAsyncScope scope;
    auto exe = co_await folly::coro::co_current_executor;

    std::atomic<uint64_t> inflight{0};
    std::atomic<uint64_t> localOps{0};
    std::atomic<uint64_t> localSuccesses{0};
    std::atomic<uint64_t> localErrors{0};

    // Track what's been synced to avoid race conditions
    uint64_t lastSyncedSuccesses = 0;
    uint64_t lastSyncedErrors = 0;

    // Send one request - matches production McrouterAdapter::coro() pattern
    auto sendOneRequest = [&]() -> folly::coro::Task<void> {
      std::string key = generateKey();
      std::string value = generateValue();

      UcbSetRequest request;
      request.key() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
      request.value() = *folly::IOBuf::copyBuffer(value);
      request.exptime() = 3600;

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::makePromiseContract<UcbSetReply>();

      clientPtr->send(
          request,
          [p = std::move(promise)](
              const UcbSetRequest&, UcbSetReply&& reply) mutable {
            p.setValue(std::move(reply));
          });

      UcbSetReply result;
      try {
        result = co_await std::move(future).within(std::chrono::seconds(10));
      } catch (const std::exception&) {
        // Request never completed (send() rejected without callback, or stuck
        // pre-send during the connection storm). Count as error and return so
        // the worker's scope.joinAsync() can't hang -> warmup completes and
        // WARMUP_DONE is sent.
        localOps++;
        localErrors++;
        co_return;
      }

      localOps++;
      if (*result.result() == carbon::Result::STORED) {
        localSuccesses++;
      } else {
        localErrors++;
        // Print only the FIRST error per worker. Any periodic sampling (even
        // 1/1000) floods stdout under a TKO storm at multi-M QPS; the
        // synchronous write() blocks on the backpressured benchpress/automark
        // pipe while holding the stdio FILE lock and glog's log_mutex, wedging
        // every worker thread -> joinAsync never returns -> BENCHMARK_DONE is
        // never sent.
        if (FLAGS_verbose && localErrors.load() == 1) {
          printf(
              "Warmup SET error (first sample): %s\n",
              carbon::resultToString(*result.result_ref()));
        }
      }
      co_return;
    };

    // Main loop - matches production LoadgenWorker::co_run()
    while (std::chrono::steady_clock::now() < endTime) {
      // Spawn requests up to current dynamic inflight limit
      uint32_t limit = currentMaxInflight.load();
      size_t currentInflight = inflight.load();
      size_t n = (currentInflight < limit) ? (limit - currentInflight) : 0;
      if (n > 0 && std::chrono::steady_clock::now() < endTime) {
        for (size_t i = 0; i < n && inflight.load() < limit; i++) {
          inflight++;
          scope.add(
              folly::coro::co_withExecutor(
                  exe, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
                    co_await sendOneRequest();
                    inflight--;
                    co_return;
                  })));
        }
      }

      // Periodically update global counters to avoid excessive atomic
      // operations
      uint64_t ops = localOps.load();
      if (ops % 100 == 0 && ops > 0) {
        uint64_t successes = localSuccesses.load();
        uint64_t errors = localErrors.load();

        // Only add the delta since last sync
        uint64_t successDelta = successes - lastSyncedSuccesses;
        uint64_t errorDelta = errors - lastSyncedErrors;

        if (successDelta > 0 || errorDelta > 0) {
          totalOps.fetch_add(successDelta + errorDelta);
          setSuccesses.fetch_add(successDelta);
          setErrors.fetch_add(errorDelta);

          lastSyncedSuccesses = successes;
          lastSyncedErrors = errors;
        }
      }

      // Yield - same as production using folly::futures::sleep
      co_await folly::futures::sleep(std::chrono::milliseconds(1));
    }

    // Warmup window over: cancel stragglers and join (deterministic drain).
    co_await scope.cancelAndJoinAsync();

    // Final update - add any remaining counts not yet synced
    uint64_t finalSuccesses = localSuccesses.load();
    uint64_t finalErrors = localErrors.load();
    uint64_t remainingSuccesses = finalSuccesses - lastSyncedSuccesses;
    uint64_t remainingErrors = finalErrors - lastSyncedErrors;

    if (remainingSuccesses > 0 || remainingErrors > 0) {
      totalOps.fetch_add(remainingSuccesses + remainingErrors);
      setSuccesses.fetch_add(remainingSuccesses);
      setErrors.fetch_add(remainingErrors);
    }
    co_return;
  };

  // Start workers - matches production LoadgenCommand::co_run() pattern
  folly::coro::AsyncScope mainScope;
  for (size_t i = 0; i < clients.size(); ++i) {
    mainScope.add(
        folly::coro::co_withExecutor(
            workerEvbs.at(i % workerEvbs.size()),
            warmupWorker(clients[i].get())));
  }

  // Block until all workers complete
  folly::coro::blockingWait(
      mainScope.joinAsync().scheduleOn(workerEvbs.front()));

  shouldStop = true;
  if (adaptiveThread.joinable()) {
    adaptiveThread.join();
  }
  if (progressThread.joinable()) {
    progressThread.join();
  }

  WarmupResults warmupResults;
  warmupResults.startTime = startTime;
  warmupResults.endTime = std::chrono::steady_clock::now();
  warmupResults.totalOps = totalOps.load();
  warmupResults.setSuccesses = setSuccesses.load();
  warmupResults.setErrors = setErrors.load();
  warmupResults.success = (setSuccesses > 0);

  auto duration = std::chrono::duration<double>(
                      warmupResults.endTime - warmupResults.startTime)
                      .count();
  double warmupQps = warmupResults.totalOps / duration;

  if (FLAGS_verbose) {
    printf(
        "Warmup completed: %lu operations in %.2f seconds (%.1f QPS)\n",
        warmupResults.totalOps,
        duration,
        warmupQps);
    printf(
        "  Successes: %lu, Errors: %lu, Success Rate: %.1f%%\n",
        warmupResults.setSuccesses,
        warmupResults.setErrors,
        warmupResults.totalOps > 0
            ? (warmupResults.setSuccesses * 100.0 / warmupResults.totalOps)
            : 0.0);
    if (!warmupResults.success) {
      printf("  WARNING: Warmup failed - no successful SET operations!\n");
    }
    fflush(stdout);
  }

  // Notify admin server that warmup is done (if connected)
  if (hasAdminConnection()) {
    printf("[Client %d] Sending WARMUP_DONE to admin server\n", clientId_);
    if (!adminConnection_->sendWarmupDone(clientId_)) {
      printf("[Client %d] Failed to send WARMUP_DONE\n", clientId_);
    } else {
      // Wait for ALL_WARMUP_DONE notification before returning
      printf(
          "[Client %d] Waiting for all clients to complete warmup...\n",
          clientId_);
      std::string notification = adminConnection_->waitForNotification(0);
      if (notification == "ALL_WARMUP_DONE") {
        printf(
            "[Client %d] All clients completed warmup, ready for benchmark\n",
            clientId_);
      } else {
        printf(
            "[Client %d] Unexpected notification while waiting for ALL_WARMUP_DONE: %s\n",
            clientId_,
            notification.c_str());
      }
    }
  }

  return warmupResults;
}

UcacheBenchClient::BenchmarkResults UcacheBenchClient::runBenchmark() {
  // Determine number of worker threads
  uint32_t numThreads = FLAGS_num_threads;
  if (numThreads == 0) {
    // Auto-detect: use half of available cores for worker threads
    numThreads = std::max(1u, std::thread::hardware_concurrency() / 2);
  }

  // Use McRouter's maximumOutstanding for backpressure (like production)
  // This is the per-client limit - McRouter will block if limit is reached
  uint32_t maxInflight = FLAGS_max_inflight;
  if (maxInflight < 1) {
    maxInflight = 1;
  }

  // Auto-concurrency: use large maxInflight for mcrouter (we control
  // concurrency via our own atomic counter), and split duration into ramp +
  // steady phases
  // Keep mcrouter's outstanding semaphore well clear of our own concurrency
  // limit. createSameThreadClient sets maximumOutstandingError, so a send that
  // races the semaphore is completed immediately with LOCAL_ERROR instead of
  // being queued — sizing both limits identically sheds load as errors.
  uint32_t mcrouterMaxOutstanding = std::max(4u * maxInflight, 500u);
  std::atomic<uint32_t> dynamicMaxInflight{maxInflight};
  std::atomic<bool> inSteadyPhase{false};
  std::atomic<uint32_t> discoveredOptimalInflight{maxInflight};

  if (FLAGS_auto_concurrency) {
    mcrouterMaxOutstanding = std::max(maxInflight, 500u);
    uint32_t initialInflight = std::max(1u, maxInflight / 10);
    dynamicMaxInflight.store(initialInflight);

    printf(
        "[auto_concurrency] Ramp phase: %us, then steady for remaining %us. "
        "Searching from %u to %u max_inflight.\n",
        FLAGS_ramp_seconds,
        FLAGS_duration_seconds > FLAGS_ramp_seconds
            ? FLAGS_duration_seconds - FLAGS_ramp_seconds
            : 0,
        initialInflight,
        maxInflight);
    fflush(stdout);
  }

  if (FLAGS_verbose) {
    printf(
        "Starting benchmark for %u seconds with %u worker threads, max_inflight=%u per client%s\n",
        FLAGS_duration_seconds,
        numThreads,
        maxInflight,
        FLAGS_auto_concurrency ? " (auto-concurrency enabled)" : "");
    fflush(stdout);
  }

  auto startTime = std::chrono::steady_clock::now();
  auto endTime = startTime + std::chrono::seconds(FLAGS_duration_seconds);

  std::atomic<bool> shouldStop{false};

  // Mutex for latency vector (std::vector is not thread-safe)
  std::mutex latenciesMutex;
  std::vector<double> allLatencies;
  allLatencies.reserve(100000 * numThreads);

  // Create clients and get EventBases for worker coroutines.
  // Two modes:
  //   1. Same-thread mode: workers run on proxy EventBases, bypassing the
  //      cross-thread message queue. One worker per proxy.
  //   2. Remote-thread mode (default): workers run on a separate thread pool,
  //      requests are dispatched to proxy threads via message queue.
  std::unique_ptr<folly::IOThreadPoolExecutor> workerPool;
  std::vector<folly::EventBase*> workerEvbs;
  std::vector<
      memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>::Pointer>
      clients;

  if (FLAGS_use_same_thread_client) {
    // Same-thread mode: one worker per proxy, running on proxy EventBases.
    // This eliminates the worker->proxy->worker cross-thread hops.
    size_t numProxies = routerInstance_->opts().num_proxies;
    clients.reserve(numProxies);
    workerEvbs.reserve(numProxies);

    for (size_t i = 0; i < numProxies; ++i) {
      auto* proxy = routerInstance_->getProxyBase(i);
      if (!proxy) {
        continue;
      }
      workerEvbs.push_back(&proxy->eventBase().getEventBase());

      auto client =
          routerInstance_->createSameThreadClient(mcrouterMaxOutstanding);
      if (client) {
        client->setProxyIndex(i);
        clients.push_back(std::move(client));
      }
    }

    if (FLAGS_verbose) {
      printf(
          "Using same-thread client mode: %zu workers on proxy EventBases\n",
          clients.size());
      fflush(stdout);
    }
  } else {
    // Remote-thread mode (original): separate worker thread pool.
    workerPool = std::make_unique<folly::IOThreadPoolExecutor>(numThreads);
    auto evbKeepAlives = workerPool->getAllEventBases();
    for (auto& ka : evbKeepAlives) {
      workerEvbs.push_back(ka.get());
    }

    clients.reserve(numThreads);
    for (uint32_t i = 0; i < numThreads; ++i) {
      auto client = routerInstance_->createClient(mcrouterMaxOutstanding);
      if (client) {
        clients.push_back(std::move(client));
      }
    }
  }

  double getRatio =
      distribution_.enabled ? distribution_.getRatio : FLAGS_get_ratio;

  // Per-worker latencies storage
  std::vector<std::vector<double>> workerLatencies(clients.size());
  std::vector<std::mutex> workerLatencyMutexes(clients.size());

  // Per-worker counters
  // Use unique_ptr to avoid vector<atomic> which is invalid C++
  // (atomic is neither copyable nor movable)
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerTotalOps;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerGetOps;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerSetOps;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerGetHits;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerGetMisses;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerGetErrors;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerSetSuccesses;
  std::vector<std::unique_ptr<std::atomic<uint64_t>>> workerSetErrors;

  workerTotalOps.reserve(clients.size());
  workerGetOps.reserve(clients.size());
  workerSetOps.reserve(clients.size());
  workerGetHits.reserve(clients.size());
  workerGetMisses.reserve(clients.size());
  workerGetErrors.reserve(clients.size());
  workerSetSuccesses.reserve(clients.size());
  workerSetErrors.reserve(clients.size());

  // Initialize per-worker counters
  for (size_t i = 0; i < clients.size(); ++i) {
    workerLatencies[i].reserve(100000);
    workerTotalOps.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerGetOps.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerSetOps.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerGetHits.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerGetMisses.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerGetErrors.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerSetSuccesses.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    workerSetErrors.push_back(std::make_unique<std::atomic<uint64_t>>(0));
  }

  // Progress monitoring thread (must be created after clients and counters)
  std::thread progressThread;
  if (FLAGS_verbose && FLAGS_progress_interval_seconds > 0) {
    progressThread = std::thread([&]() {
      while (!shouldStop.load() && std::chrono::steady_clock::now() < endTime) {
        std::this_thread::sleep_for(
            std::chrono::seconds(FLAGS_progress_interval_seconds));
        if (shouldStop.load()) {
          break;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - startTime).count();

        // Sum per-worker counters for progress display
        uint64_t ops = 0;
        uint64_t gets = 0;
        uint64_t sets = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t gErrs = 0;
        uint64_t sSucc = 0;
        uint64_t sErrs = 0;

        for (size_t i = 0; i < clients.size(); ++i) {
          ops += workerTotalOps[i]->load();
          gets += workerGetOps[i]->load();
          sets += workerSetOps[i]->load();
          hits += workerGetHits[i]->load();
          misses += workerGetMisses[i]->load();
          gErrs += workerGetErrors[i]->load();
          sSucc += workerSetSuccesses[i]->load();
          sErrs += workerSetErrors[i]->load();
        }

        double avgQps = elapsed > 0 ? ops / elapsed : 0;

        printf(
            "Benchmark progress: %.1fs elapsed, %lu ops (%.1f QPS avg), GET: %lu (hits=%lu misses=%lu err=%lu), SET: %lu (succ=%lu err=%lu)\n",
            elapsed,
            ops,
            avgQps,
            gets,
            hits,
            misses,
            gErrs,
            sets,
            sSucc,
            sErrs);
        fflush(stdout);
      }
    });
  }

  // Per-worker inflight counters for auto-concurrency throttling
  std::vector<std::unique_ptr<std::atomic<uint32_t>>> workerInflight;
  for (size_t i = 0; i < clients.size(); ++i) {
    workerInflight.push_back(std::make_unique<std::atomic<uint32_t>>(0));
  }

  // AIMD auto-concurrency controller thread
  std::thread autoConcurrencyThread;
  if (FLAGS_auto_concurrency) {
    autoConcurrencyThread = std::thread([&]() {
      constexpr auto kCheckInterval = std::chrono::seconds(2);
      auto rampEnd = startTime + std::chrono::seconds(FLAGS_ramp_seconds);

      uint64_t prevOps = 0;
      double bestQps = 0;
      uint32_t bestInflight = dynamicMaxInflight.load();
      uint32_t peakInflight = dynamicMaxInflight.load();

      while (!shouldStop.load() && std::chrono::steady_clock::now() < endTime) {
        std::this_thread::sleep_for(kCheckInterval);
        if (shouldStop.load()) {
          break;
        }

        auto now = std::chrono::steady_clock::now();
        bool ramping = now < rampEnd;

        // Calculate current window QPS
        uint64_t curOps = 0;
        for (size_t i = 0; i < clients.size(); ++i) {
          curOps += workerTotalOps[i]->load();
        }
        uint64_t windowOps = curOps - prevOps;
        prevOps = curOps;
        double windowQps = windowOps / 2.0; // 2-second window

        uint32_t cur = dynamicMaxInflight.load();

        if (ramping) {
          // Track best QPS and corresponding inflight
          if (windowQps > bestQps) {
            bestQps = windowQps;
            bestInflight = cur;
          }

          // Additive increase during ramp: grow by 10% or at least 1
          uint32_t increment = std::max(1u, cur / 10);
          uint32_t next = std::min(maxInflight, cur + increment);
          dynamicMaxInflight.store(next);

          if (windowQps < bestQps * 0.9 && cur > bestInflight) {
            // QPS dropped >10% — we overshot, snap back
            next = bestInflight;
            dynamicMaxInflight.store(next);
            printf(
                "[auto_concurrency] QPS dropped (%.0f < %.0f), snapping back to %u\n",
                windowQps,
                bestQps,
                next);
          } else if (FLAGS_verbose) {
            printf(
                "[auto_concurrency] ramp: inflight=%u, QPS=%.0f (best=%.0f@%u)\n",
                cur,
                windowQps,
                bestQps,
                bestInflight);
          }
          fflush(stdout);
        } else if (!inSteadyPhase.load()) {
          // Transition to steady state
          peakInflight = bestInflight;
          uint32_t steadyInflight =
              static_cast<uint32_t>(peakInflight * FLAGS_target_utilization);
          steadyInflight = std::max(1u, steadyInflight);
          dynamicMaxInflight.store(steadyInflight);
          discoveredOptimalInflight.store(steadyInflight);
          inSteadyPhase.store(true);

          printf(
              "[auto_concurrency] Steady state: peak inflight=%u (%.0f QPS), "
              "target_utilization=%.0f%%, using inflight=%u\n",
              peakInflight,
              bestQps,
              FLAGS_target_utilization * 100.0,
              steadyInflight);
          fflush(stdout);
        }
      }
    });
  }

  // Worker coroutine - matches production LoadgenWorker::co_run() pattern
  auto benchmarkWorker =
      [&](size_t workerId,
          memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>*
              clientPtr) -> folly::coro::Task<void> {
    // CancellableAsyncScope (not AsyncScope) so that when the measurement
    // window ends we CANCEL any still-outstanding requests instead of waiting
    // for them indefinitely. A small number of in-flight requests at endTime
    // can have responses that never arrive (and the per-request timeout's timer
    // may sit on a momentarily-saturated proxy event base), which otherwise
    // hangs joinAsync forever -> the client never sends BENCHMARK_DONE -> the
    // whole run stalls. Cancelling at window-end is the correct semantics.
    folly::coro::CancellableAsyncScope scope;
    auto exe = co_await folly::coro::co_current_executor;

    // Send one GET request - matches production McrouterAdapter::coro() pattern
    auto sendGetRequest = [&]() -> folly::coro::Task<void> {
      auto opStartTime = std::chrono::steady_clock::now();
      std::string key = generateKey();

      UcbGetRequest request;
      request.key() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::makePromiseContract<UcbGetReply>();

      // Latency stage sampling. Average RPC latency measured ~27ms against a
      // server that is idle in epoll with no queue anywhere, so the time has to
      // be attributed to a stage rather than guessed at. Three timestamps split
      // it: send() duration is synchronous mcrouter routing, t1-t0 is transport
      // plus server plus response arrival, and t2-t1 is how long the completion
      // waited for this EventBase to run it. Sampled 1/10000 so the timestamps
      // themselves do not perturb the measurement.
      const bool sampleLat = (tLatSampleCounter++ % kLatSampleEvery) == 0;
      std::chrono::steady_clock::time_point t0, tSent;
      // Only allocated on sampled requests. Allocating unconditionally would
      // add a heap allocation and refcount traffic to every request.
      std::shared_ptr<std::atomic<int64_t>> cbEntry;
      if (sampleLat) {
        cbEntry = std::make_shared<std::atomic<int64_t>>(0);
        t0 = std::chrono::steady_clock::now();
      }

      clientPtr->send(
          request,
          [p = std::move(promise), sampleLat, cbEntry](
              const UcbGetRequest&, UcbGetReply&& reply) mutable {
            if (sampleLat) {
              cbEntry->store(
                  std::chrono::steady_clock::now().time_since_epoch().count(),
                  std::memory_order_relaxed);
            }
            p.setValue(std::move(reply));
          });
      if (sampleLat) {
        tSent = std::chrono::steady_clock::now();
      }

      UcbGetReply result;
      try {
        result = co_await std::move(future).within(std::chrono::seconds(10));
      } catch (const std::exception&) {
        // Request never completed; count as a GET error and return so the
        // measurement worker's scope.joinAsync() can't hang.
        if (sampleLat) {
          // Timed-out requests are precisely the interesting ones, so record
          // that this sample was lost rather than letting it vanish and skew
          // the effective sampling rate.
          gLatLostSamples.fetch_add(1, std::memory_order_relaxed);
        }
        workerTotalOps[workerId]->fetch_add(1);
        workerGetOps[workerId]->fetch_add(1);
        workerGetErrors[workerId]->fetch_add(1);
        co_return;
      }

      auto opEndTime = std::chrono::steady_clock::now();

      if (sampleLat) {
        auto ns = [](auto d) {
          return std::chrono::duration_cast<std::chrono::nanoseconds>(d)
              .count();
        };
        int64_t cb = cbEntry->load(std::memory_order_relaxed);
        gLatSendNs.fetch_add(ns(tSent - t0), std::memory_order_relaxed);
        if (cb != 0) {
          auto cbTp = std::chrono::steady_clock::time_point(
              std::chrono::steady_clock::duration(cb));
          gLatTransportNs.fetch_add(ns(cbTp - t0), std::memory_order_relaxed);
          gLatResumeNs.fetch_add(
              ns(opEndTime - cbTp), std::memory_order_relaxed);
        }
        gLatTotalNs.fetch_add(ns(opEndTime - t0), std::memory_order_relaxed);
        gLatSamples.fetch_add(1, std::memory_order_relaxed);
      }
      auto latencyMs =
          std::chrono::duration<double, std::milli>(opEndTime - opStartTime)
              .count();

      {
        std::lock_guard<std::mutex> lock(workerLatencyMutexes[workerId]);
        workerLatencies[workerId].push_back(latencyMs);
      }

      workerTotalOps[workerId]->fetch_add(1);
      workerGetOps[workerId]->fetch_add(1);

      if (*result.result() == carbon::Result::FOUND) {
        workerGetHits[workerId]->fetch_add(1);
      } else if (*result.result() == carbon::Result::NOTFOUND) {
        workerGetMisses[workerId]->fetch_add(1);

        // SET on GET miss to simulate real cache warming behavior
        // This matches the behavior from the old synchronous version
        std::string value = generateValue();

        UcbSetRequest setRequest;
        setRequest.key() = carbon::Keys<folly::IOBuf>(
            std::move(*folly::IOBuf::copyBuffer(key)));
        setRequest.value() = *folly::IOBuf::copyBuffer(value);
        setRequest.exptime() = 3600;

        if (FLAGS_enable_random_source_ip) {
          uint8_t randomOctet = folly::Random::rand32(1, 255);
          std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
          try {
            folly::IPAddress sourceIp(ipStr);
            setRequest.setSourceIpAddr(sourceIp);
          } catch (const std::exception&) {
          }
        }

        auto [setPromise, setFuture] =
            folly::makePromiseContract<UcbSetReply>();

        clientPtr->send(
            setRequest,
            [p = std::move(setPromise)](
                const UcbSetRequest&, UcbSetReply&& reply) mutable {
              p.setValue(std::move(reply));
            });

        UcbSetReply setResult;
        try {
          setResult =
              co_await std::move(setFuture).within(std::chrono::seconds(10));
        } catch (const std::exception&) {
          workerSetOps[workerId]->fetch_add(1);
          workerSetErrors[workerId]->fetch_add(1);
          co_return;
        }

        workerSetOps[workerId]->fetch_add(1);
        if (*setResult.result() == carbon::Result::STORED) {
          workerSetSuccesses[workerId]->fetch_add(1);
        } else {
          workerSetErrors[workerId]->fetch_add(1);
          if (FLAGS_verbose && workerSetErrors[workerId]->load() == 1) {
            printf(
                "Benchmark SET error (on GET miss, first sample): %s\n",
                carbon::resultToString(*setResult.result_ref()));
          }
        }
      } else {
        workerGetErrors[workerId]->fetch_add(1);
        if (FLAGS_verbose && workerGetErrors[workerId]->load() == 1) {
          printf(
              "Benchmark GET error (first sample): %s\n",
              carbon::resultToString(*result.result_ref()));
        }
      }

      co_return;
    };

    // Send one SET request - matches production McrouterAdapter::coro() pattern
    auto sendSetRequest = [&]() -> folly::coro::Task<void> {
      auto opStartTime = std::chrono::steady_clock::now();
      std::string key = generateKey();
      std::string value = generateValue();

      UcbSetRequest request;
      request.key() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
      request.value() = *folly::IOBuf::copyBuffer(value);
      request.exptime() = 3600;

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::makePromiseContract<UcbSetReply>();

      clientPtr->send(
          request,
          [p = std::move(promise)](
              const UcbSetRequest&, UcbSetReply&& reply) mutable {
            p.setValue(std::move(reply));
          });

      UcbSetReply result;
      try {
        result = co_await std::move(future).within(std::chrono::seconds(10));
      } catch (const std::exception&) {
        workerTotalOps[workerId]->fetch_add(1);
        workerSetOps[workerId]->fetch_add(1);
        workerSetErrors[workerId]->fetch_add(1);
        co_return;
      }

      auto opEndTime = std::chrono::steady_clock::now();
      auto latencyMs =
          std::chrono::duration<double, std::milli>(opEndTime - opStartTime)
              .count();

      {
        std::lock_guard<std::mutex> lock(workerLatencyMutexes[workerId]);
        workerLatencies[workerId].push_back(latencyMs);
      }

      workerTotalOps[workerId]->fetch_add(1);
      workerSetOps[workerId]->fetch_add(1);

      if (*result.result() == carbon::Result::STORED) {
        workerSetSuccesses[workerId]->fetch_add(1);
      } else {
        workerSetErrors[workerId]->fetch_add(1);
        if (FLAGS_verbose && workerSetErrors[workerId]->load() == 1) {
          printf(
              "Benchmark SET error (first sample): %s\n",
              carbon::resultToString(*result.result_ref()));
        }
      }

      co_return;
    };

    // Persistent completion-driven lanes: each lane holds exactly one request
    // in flight and issues its next one the instant the previous completes.
    //
    // The previous design topped every worker back up to maxInflight and then
    // slept 1ms. That sleep is a timer on the proxy EventBase, which is also
    // serving this proxy's connections, so the wakeup slips under load: offered
    // load arrives as millisecond bursts and queues inflate rather than
    // pipelining. Measured residence was ~60ms at maxInflight=150 and ~258ms at
    // 600 — which is why raising the window lowered throughput.
    //
    // Lanes cannot burst, so mcrouter's outstanding semaphore is never raced
    // (see mcrouterMaxOutstanding, which is now decoupled from maxInflight;
    // createSameThreadClient sets maximumOutstandingError, so a raced send is
    // failed with LOCAL_ERROR rather than queued, silently shedding load).
    auto& myInflight = *workerInflight[workerId];
    const uint32_t laneCount = maxInflight;

    for (uint32_t lane = 0; lane < laneCount; ++lane) {
      scope.add(
          folly::coro::co_withExecutor(
              exe,
              folly::coro::co_invoke([&, lane]() -> folly::coro::Task<void> {
                // Stagger each lane's first request. Every lane otherwise
                // starts together and then reissues on completion, so the
                // whole cohort can stay phase-locked: the server sees bursts
                // separated by idle gaps rather than a smooth arrival process,
                // which caps throughput while leaving IO threads in epoll_wait.
                // The offset is deterministic per lane, spread over roughly one
                // observed response time, and only shifts the first request --
                // concurrency and the steady-state loop are unchanged.
                if (FLAGS_lane_phase_stagger_us > 0) {
                  co_await folly::futures::sleep(
                      std::chrono::microseconds(
                          (static_cast<uint64_t>(lane) * 2654435761ULL) %
                          FLAGS_lane_phase_stagger_us));
                }
                while (std::chrono::steady_clock::now() < endTime) {
                  // auto_concurrency shrinks the active window by parking
                  // the highest-numbered lanes.
                  if (FLAGS_auto_concurrency &&
                      lane >= dynamicMaxInflight.load()) {
                    co_await folly::futures::sleep(
                        std::chrono::milliseconds(1));
                    continue;
                  }
                  myInflight.fetch_add(1);
                  if (folly::Random::randDouble01() < getRatio) {
                    co_await sendGetRequest();
                  } else {
                    co_await sendSetRequest();
                  }
                  myInflight.fetch_sub(1);
                }
                co_return;
              })));
    }

    // Measurement window is over: cancel any still-outstanding requests and
    // join. This drains deterministically instead of hanging on stragglers.
    co_await scope.cancelAndJoinAsync();

    // No need to sync to global counters - they'll be summed at the end
    co_return;
  };

  // Start workers - matches production LoadgenCommand::co_run() pattern
  folly::coro::AsyncScope mainScope;
  for (size_t i = 0; i < clients.size(); ++i) {
    mainScope.add(
        folly::coro::co_withExecutor(
            workerEvbs.at(i % workerEvbs.size()),
            benchmarkWorker(i, clients[i].get())));
  }

  // Block until all workers complete
  folly::coro::blockingWait(
      mainScope.joinAsync().scheduleOn(workerEvbs.front()));

  shouldStop = true;
  if (progressThread.joinable()) {
    progressThread.join();
  }
  if (autoConcurrencyThread.joinable()) {
    autoConcurrencyThread.join();
  }

  // Merge all worker latencies
  for (size_t i = 0; i < clients.size(); ++i) {
    std::lock_guard<std::mutex> lock(latenciesMutex);
    allLatencies.insert(
        allLatencies.end(),
        workerLatencies[i].begin(),
        workerLatencies[i].end());
  }

  BenchmarkResults results;
  results.startTime = startTime;
  results.endTime = std::chrono::steady_clock::now();

  // Sum per-worker counters to get final totals
  results.totalOps = 0;
  results.getOps = 0;
  results.setOps = 0;
  results.getHits = 0;
  results.getMisses = 0;
  results.getErrors = 0;
  results.setSuccesses = 0;
  results.setErrors = 0;

  for (size_t i = 0; i < clients.size(); ++i) {
    results.totalOps += workerTotalOps[i]->load();
    results.getOps += workerGetOps[i]->load();
    results.setOps += workerSetOps[i]->load();
    results.getHits += workerGetHits[i]->load();
    results.getMisses += workerGetMisses[i]->load();
    results.getErrors += workerGetErrors[i]->load();
    results.setSuccesses += workerSetSuccesses[i]->load();
    results.setErrors += workerSetErrors[i]->load();
  }

  results.latencies = std::move(allLatencies);

  // Dump mcrouter pipeline stats for profiling
  if (routerInstance_) {
    using facebook::memcache::mcrouter::num_stats;
    using facebook::memcache::mcrouter::prepare_stats;
    using facebook::memcache::mcrouter::stat_t;

    stat_t stats[num_stats];
    facebook::memcache::mcrouter::init_stats(stats);
    prepare_stats(*routerInstance_, stats);

    printf("\n=== McRouter Pipeline Stats ===\n");

    // Key timing stats
    auto printDoubleStat = [&](const char* label,
                               facebook::memcache::mcrouter::stat_name_t s) {
      double val = folly::make_atomic_ref(stats[s].data.dbl)
                       .load(std::memory_order_relaxed);
      printf("  %-40s: %.2f\n", label, val);
    };
    auto printUint64Stat = [&](const char* label,
                               facebook::memcache::mcrouter::stat_name_t s) {
      uint64_t val = folly::make_atomic_ref(stats[s].data.uint64)
                         .load(std::memory_order_relaxed);
      printf("  %-40s: %lu\n", label, val);
    };

    printDoubleStat(
        "duration_us (avg RPC latency)",
        facebook::memcache::mcrouter::duration_us_stat);
    printDoubleStat(
        "duration_get_us", facebook::memcache::mcrouter::duration_get_us_stat);
    printDoubleStat(
        "duration_update_us",
        facebook::memcache::mcrouter::duration_update_us_stat);
    printDoubleStat(
        "processing_time_us (mcrouter only)",
        facebook::memcache::mcrouter::processing_time_us_stat);
    printDoubleStat(
        "destination_batch_size",
        facebook::memcache::mcrouter::destination_batch_size_stat);
    printUint64Stat(
        "destination_pending_reqs",
        facebook::memcache::mcrouter::destination_pending_reqs_stat);
    printUint64Stat(
        "destination_inflight_reqs",
        facebook::memcache::mcrouter::destination_inflight_reqs_stat);
    printUint64Stat(
        "destination_max_pending_reqs",
        facebook::memcache::mcrouter::destination_max_pending_reqs_stat);
    printUint64Stat(
        "destination_max_inflight_reqs",
        facebook::memcache::mcrouter::destination_max_inflight_reqs_stat);
    printUint64Stat(
        "outstanding_route_get_reqs_queued",
        facebook::memcache::mcrouter::outstanding_route_get_reqs_queued_stat);
    printUint64Stat(
        "outstanding_route_update_reqs_queued",
        facebook::memcache::mcrouter::
            outstanding_route_update_reqs_queued_stat);

    printf("=== End McRouter Pipeline Stats ===\n\n");

    // Also write to file for retrieval via sush2
    FILE* statsFile = fopen("/tmp/mcrouter_stats.txt", "w");
    if (statsFile) {
      auto writeDoubleStat = [&](const char* label,
                                 facebook::memcache::mcrouter::stat_name_t s) {
        double val = folly::make_atomic_ref(stats[s].data.dbl)
                         .load(std::memory_order_relaxed);
        fprintf(statsFile, "%-40s: %.2f\n", label, val);
      };
      auto writeUint64Stat = [&](const char* label,
                                 facebook::memcache::mcrouter::stat_name_t s) {
        uint64_t val = folly::make_atomic_ref(stats[s].data.uint64)
                           .load(std::memory_order_relaxed);
        fprintf(statsFile, "%-40s: %lu\n", label, val);
      };

      writeDoubleStat(
          "duration_us (avg RPC latency)",
          facebook::memcache::mcrouter::duration_us_stat);
      writeDoubleStat(
          "duration_get_us",
          facebook::memcache::mcrouter::duration_get_us_stat);
      writeDoubleStat(
          "duration_update_us",
          facebook::memcache::mcrouter::duration_update_us_stat);
      writeDoubleStat(
          "processing_time_us (mcrouter only)",
          facebook::memcache::mcrouter::processing_time_us_stat);
      writeDoubleStat(
          "destination_batch_size",
          facebook::memcache::mcrouter::destination_batch_size_stat);
      writeUint64Stat(
          "destination_pending_reqs",
          facebook::memcache::mcrouter::destination_pending_reqs_stat);
      writeUint64Stat(
          "destination_inflight_reqs",
          facebook::memcache::mcrouter::destination_inflight_reqs_stat);
      writeUint64Stat(
          "destination_max_pending_reqs",
          facebook::memcache::mcrouter::destination_max_pending_reqs_stat);
      writeUint64Stat(
          "destination_max_inflight_reqs",
          facebook::memcache::mcrouter::destination_max_inflight_reqs_stat);
      writeUint64Stat(
          "outstanding_route_get_reqs_queued",
          facebook::memcache::mcrouter::outstanding_route_get_reqs_queued_stat);
      writeUint64Stat(
          "outstanding_route_update_reqs_queued",
          facebook::memcache::mcrouter::
              outstanding_route_update_reqs_queued_stat);
      fclose(statsFile);
    }
  }

  // Notify admin server that benchmark is done (if connected)
  reportLatencyBreakdown();
  if (hasAdminConnection()) {
    printf("[Client %d] Sending BENCHMARK_DONE to admin server\n", clientId_);
    if (!adminConnection_->sendBenchmarkDone(clientId_)) {
      printf("[Client %d] Failed to send BENCHMARK_DONE\n", clientId_);
    } else {
      // Wait for ALL_DONE notification (server is printing results)
      printf("[Client %d] Waiting for server to finish...\n", clientId_);
      std::string notification = adminConnection_->waitForNotification(0);
      if (notification == "ALL_DONE") {
        printf("[Client %d] Server finished, benchmark complete\n", clientId_);
      } else if (!notification.empty()) {
        printf(
            "[Client %d] Received notification: %s\n",
            clientId_,
            notification.c_str());
      }
    }
    // Disconnect from admin server
    adminConnection_->disconnect();
  }

  return results;
}

std::string UcacheBenchClient::generateKey() {
  uint32_t keyId;

  if (FLAGS_zipfian) {
    // Use Zipfian distribution for hot-key access pattern
    // Thread-local generator to avoid contention
    thread_local std::unique_ptr<ZipfianGenerator> zipfGen;
    if (!zipfGen || zipfGen->getSkew() != FLAGS_zipfian_skew) {
      zipfGen = std::make_unique<ZipfianGenerator>(
          FLAGS_key_count, FLAGS_zipfian_skew);
    }

    if (FLAGS_hot_key_ratio > 0.0) {
      // Hybrid mode: hot_key_ratio of keys get extra access frequency
      // This simulates production's hot key detection (25x frequency)
      uint32_t hotKeyCount =
          static_cast<uint32_t>(FLAGS_key_count * FLAGS_hot_key_ratio);
      if (hotKeyCount < 1) {
        hotKeyCount = 1;
      }

      // 80% of accesses go to hot keys (Pareto principle)
      if (folly::Random::randDouble01() < 0.8) {
        // Access a hot key (from the first hotKeyCount keys)
        keyId = folly::Random::rand32(hotKeyCount);
      } else {
        // Access a cold key (uniform random from remaining keys)
        keyId =
            hotKeyCount + folly::Random::rand32(FLAGS_key_count - hotKeyCount);
      }
    } else {
      // Pure Zipfian distribution
      // Scramble the key ID to avoid sequential access patterns
      // This matches YCSB's ScrambledZipfianGenerator behavior
      uint64_t zipfValue = zipfGen->next();
      keyId =
          static_cast<uint32_t>((zipfValue * 0x9E3779B9ULL) % FLAGS_key_count);
    }
  } else {
    // Original uniform random distribution
    keyId = folly::Random::rand32(FLAGS_key_count);
  }

  if (distribution_.enabled) {
    // Use production distribution for key size
    double avgKeySize = (folly::Random::randDouble01() < distribution_.getRatio)
        ? distribution_.getKeySizeAvg
        : distribution_.setKeySizeAvg;

    // Generate key with approximately the right size
    std::string baseKey = fmt::format("key_{:08d}", keyId);

    // Pad or trim to match average key size
    int32_t targetSize = static_cast<int32_t>(avgKeySize);
    int32_t currentSize = static_cast<int32_t>(baseKey.size());

    if (currentSize < targetSize) {
      // Pad with deterministic characters based on keyId
      // This ensures the same keyId always generates the same full key
      for (int32_t i = 0; i < (targetSize - currentSize); ++i) {
        // Use keyId to generate deterministic padding
        baseKey.push_back('a' + ((keyId + i) % 26));
      }
    } else if (currentSize > targetSize && targetSize > 8) {
      // Trim (but keep at least "key_0000" format)
      baseKey = baseKey.substr(0, targetSize);
    }

    return baseKey;
  }

  // Default behavior: fixed format key
  return fmt::format("key_{:08d}", keyId);
}

namespace {

// Values are built from a small token vocabulary rather than random bytes.
// Real cache payloads are serialized records and compress several-fold;
// uniformly random bytes are incompressible, which would understate the cost
// of the server's reply-compression codec and misrepresent the memory traffic
// of copying them. Deterministic in the key hash so a given key keeps stable
// content across SETs.
constexpr const char* kValueTokens[] = {
    "user_id",      "session",  "timestamp",  "region",     "payload",
    "metadata",     "version",  "checksum",   "attributes", "profile",
    "account",      "status",   "created_at", "updated_at", "expires",
    "content_type", "encoding", "compressed", "shard",      "replica",
    "true",         "false",    "null",       "value",
};
constexpr size_t kNumValueTokens =
    sizeof(kValueTokens) / sizeof(kValueTokens[0]);

std::string makeCompressibleValue(uint32_t valueSize, uint64_t seed) {
  std::string value;
  value.reserve(valueSize + 16);
  uint64_t h = seed ? seed : 0x9E3779B97F4A7C15ULL;
  while (value.size() < valueSize) {
    h = folly::hash::twang_mix64(h);
    value += kValueTokens[h % kNumValueTokens];
    value.push_back((h & 1) ? '=' : ',');
  }
  value.resize(valueSize);
  return value;
}
} // namespace

std::string UcacheBenchClient::generateValue() {
  if (distribution_.enabled) {
    // Sample value size from production percentile distribution
    uint32_t valueSize = sampleFromPercentiles(
        distribution_.setValueSizeP50,
        distribution_.setValueSizeP75,
        distribution_.setValueSizeP95,
        distribution_.setValueSizeP99);

    // Ensure reasonable bounds (match server's max alloc size of 64KB)
    if (valueSize < 1) {
      valueSize = 1;
    }
    if (valueSize > 65536) {
      valueSize = 65536; // Cap at 64KB to match server's max allocation
    }

    return makeCompressibleValue(valueSize, folly::Random::rand64());
  }

  // Default behavior: random size between min and max
  uint32_t valueSize =
      folly::Random::rand32(FLAGS_value_size_min, FLAGS_value_size_max + 1);

  return makeCompressibleValue(valueSize, folly::Random::rand64());
}

// mcrouter operations using UcacheBench service
void UcacheBenchClient::sendUcbGetRequestSync(
    facebook::memcache::mcrouter::CarbonRouterClient<
        UcacheBenchRouterInfo>::Pointer& client,
    const std::string& key,
    const std::function<void(UcbGetReply&&)>& callback) {
  UcbGetRequest request;
  request.key() =
      carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));

  // Set random source IP if enabled for connection fanout
  if (FLAGS_enable_random_source_ip) {
    // Generate a random IPv6 address in the format ::ffff:192.0.2.X
    // Using IPv4-mapped IPv6 addresses for simplicity
    uint8_t randomOctet = folly::Random::rand32(1, 255);
    std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
    try {
      folly::IPAddress sourceIp(ipStr);
      request.setSourceIpAddr(sourceIp);
    } catch (const std::exception& e) {
      // Log and continue without source IP on error
      if (FLAGS_verbose) {
        printf("Warning: Failed to set source IP: %s\n", e.what());
      }
    }
  }

  // Use mcrouter async client with synchronization
  folly::fibers::Baton baton;
  UcbGetReply result;
  std::exception_ptr exceptionPtr = nullptr;

  bool success =
      client->send(request, [&](const UcbGetRequest&, UcbGetReply&& reply) {
        result = std::move(reply);
        baton.post();
      });

  if (!success) {
    // Failed to send - populate error message (result defaults to UNKNOWN)
    result.message() = "Failed to send GET request to mcrouter";
    callback(std::move(result));
    return;
  }

  // Wait for response
  baton.wait();
  callback(std::move(result));
}

void UcacheBenchClient::sendUcbSetRequestSync(
    facebook::memcache::mcrouter::CarbonRouterClient<
        UcacheBenchRouterInfo>::Pointer& client,
    const std::string& key,
    const std::string& value,
    const std::function<void(UcbSetReply&&)>& callback) {
  UcbSetRequest request;
  request.key() =
      carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
  request.value() = *folly::IOBuf::copyBuffer(value);
  request.exptime() = 3600; // 1 hour default TTL

  // Set random source IP if enabled for connection fanout
  if (FLAGS_enable_random_source_ip) {
    // Generate a random IPv6 address in the format ::ffff:192.0.2.X
    // Using IPv4-mapped IPv6 addresses for simplicity
    uint8_t randomOctet = folly::Random::rand32(1, 255);
    std::string ipStr = fmt::format("::ffff:192.0.2.{}", randomOctet);
    try {
      folly::IPAddress sourceIp(ipStr);
      request.setSourceIpAddr(sourceIp);
    } catch (const std::exception& e) {
      // Log and continue without source IP on error
      if (FLAGS_verbose) {
        printf("Warning: Failed to set source IP: %s\n", e.what());
      }
    }
  }

  // Use mcrouter async client with synchronization
  folly::fibers::Baton baton;
  UcbSetReply result;

  bool success =
      client->send(request, [&](const UcbSetRequest&, UcbSetReply&& reply) {
        result = std::move(reply);
        baton.post();
      });

  if (!success) {
    // Failed to send - populate error message (result defaults to UNKNOWN)
    result.message() = "Failed to send SET request to mcrouter";
    callback(std::move(result));
    return;
  }

  // Wait for response
  baton.wait();
  callback(std::move(result));
}

void UcacheBenchClient::printResults(const BenchmarkResults& results) {
  auto duration =
      std::chrono::duration<double>(results.endTime - results.startTime)
          .count();
  double qps = results.totalOps / duration;

  // Calculate latency percentiles
  std::vector<double> sortedLatencies = results.latencies;
  std::sort(sortedLatencies.begin(), sortedLatencies.end());

  double p50 = 0.0, p95 = 0.0, p99 = 0.0, p999 = 0.0;
  if (!sortedLatencies.empty()) {
    p50 = sortedLatencies[sortedLatencies.size() * 0.50];
    p95 = sortedLatencies[sortedLatencies.size() * 0.95];
    p99 = sortedLatencies[sortedLatencies.size() * 0.99];
    p999 = sortedLatencies[sortedLatencies.size() * 0.999];
  }

  double hitRatio = 0.0;
  if (results.getOps > 0) {
    hitRatio = static_cast<double>(results.getHits) / results.getOps * 100.0;
  }

  // Print warmup results first
  printf("\n=== UcacheBench Results ===\n");

  // Warmup Summary
  printf("WARMUP PHASE:\n");
  if (results.warmupResults.totalOps == 0) {
    printf("  Status: Disabled (warmup_seconds=0)\n");
  } else {
    auto warmupDuration =
        std::chrono::duration<double>(
            results.warmupResults.endTime - results.warmupResults.startTime)
            .count();
    double warmupQps = results.warmupResults.totalOps / warmupDuration;
    double warmupSuccessRate = results.warmupResults.totalOps > 0
        ? (results.warmupResults.setSuccesses * 100.0 /
           results.warmupResults.totalOps)
        : 0.0;

    printf(
        "  Status: %s\n",
        results.warmupResults.success ? "✓ SUCCESS" : "✗ FAILED");
    printf("  Duration: %.2f seconds\n", warmupDuration);
    printf(
        "  Operations: %lu (%.1f QPS)\n",
        results.warmupResults.totalOps,
        warmupQps);
    printf("  SET Successes: %lu\n", results.warmupResults.setSuccesses);
    printf("  SET Errors: %lu\n", results.warmupResults.setErrors);
    printf("  Success Rate: %.1f%%\n", warmupSuccessRate);

    if (!results.warmupResults.success) {
      printf("  ⚠️  WARNING: Cache may not be properly seeded for testing!\n");
    }
  }
  printf("\n");

  // Benchmark Summary
  printf("BENCHMARK PHASE:\n");
  printf("  Duration: %.2f seconds\n", duration);
  printf("  Total Operations: %lu\n", results.totalOps);
  printf("  QPS: %.1f\n", qps);
  printf("\n");

  printf("GET Operations: %lu\n", results.getOps);
  printf("  Hits: %lu\n", results.getHits);
  printf("  Misses: %lu\n", results.getMisses);
  printf("  Errors: %lu\n", results.getErrors);
  printf("  Hit Ratio: %.2f%%\n", hitRatio);
  printf("\n");

  printf("SET Operations: %lu\n", results.setOps);
  printf("  Successes: %lu\n", results.setSuccesses);
  printf("  Errors: %lu\n", results.setErrors);
  printf("\n");

  printf("Latency Percentiles (ms):\n");
  printf("  P50: %.2f\n", p50);
  printf("  P95: %.2f\n", p95);
  printf("  P99: %.2f\n", p99);
  printf("  P99.9: %.2f\n", p999);
  printf("\n");

  // Overall summary
  if (results.warmupResults.totalOps > 0 && !results.warmupResults.success) {
    printf(
        "🔍 ANALYSIS: Poor warmup may explain low hit ratio (%.2f%%). Consider:\n",
        hitRatio);
    printf("   - Increasing warmup duration (--warmup_seconds)\n");
    printf("   - Checking server-side SET operation handling\n");
    printf("   - Verifying cache capacity and eviction policies\n");
  } else if (results.warmupResults.totalOps > 0 && hitRatio < 10.0) {
    printf(
        "🔍 ANALYSIS: Low hit ratio (%.2f%%) despite successful warmup. Consider:\n",
        hitRatio);
    printf("   - Key distribution vs cache capacity\n");
    printf("   - Cache eviction policies\n");
    printf("   - TTL settings\n");
  }
}

void UcacheBenchClient::loadTrafficDistribution(const std::string& configFile) {
  // Read JSON config file
  std::ifstream file(configFile);
  if (!file.is_open()) {
    throw std::runtime_error(
        fmt::format("Failed to open distribution config: {}", configFile));
  }

  std::string content(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // Parse JSON using folly::dynamic
  folly::dynamic config;
  try {
    config = folly::parseJson(content);
  } catch (const std::exception& e) {
    throw std::runtime_error(
        fmt::format("Failed to parse JSON config: {}", e.what()));
  }

  // Load configuration values
  distribution_.getRatio = config["get_ratio"].asDouble();
  distribution_.getKeySizeAvg = config["get_key_size_avg"].asDouble();
  distribution_.getResponseSizeAvg = config["get_response_size_avg"].asDouble();
  distribution_.getResponseSizeP50 = config["get_response_size_p50"].asDouble();
  distribution_.getResponseSizeP75 = config["get_response_size_p75"].asDouble();
  distribution_.getResponseSizeP95 = config["get_response_size_p95"].asDouble();
  distribution_.getResponseSizeP99 = config["get_response_size_p99"].asDouble();

  distribution_.setKeySizeAvg = config["set_key_size_avg"].asDouble();
  distribution_.setValueSizeAvg = config["set_value_size_avg"].asDouble();
  distribution_.setValueSizeP50 = config["set_value_size_p50"].asDouble();
  distribution_.setValueSizeP75 = config["set_value_size_p75"].asDouble();
  distribution_.setValueSizeP95 = config["set_value_size_p95"].asDouble();
  distribution_.setValueSizeP99 = config["set_value_size_p99"].asDouble();
}

uint32_t UcacheBenchClient::sampleFromPercentiles(
    double p50,
    double p75,
    double p95,
    double p99) const {
  // Sample percentile distribution using piecewise linear approximation
  // This creates a rough distribution that matches the percentiles

  double rand = folly::Random::randDouble01();

  // Map random value to percentile ranges
  if (rand < 0.50) {
    // 0-50%: linear interpolation between 0 and p50
    return static_cast<uint32_t>(rand / 0.50 * p50);
  } else if (rand < 0.75) {
    // 50-75%: linear interpolation between p50 and p75
    double t = (rand - 0.50) / 0.25;
    return static_cast<uint32_t>(p50 + t * (p75 - p50));
  } else if (rand < 0.95) {
    // 75-95%: linear interpolation between p75 and p95
    double t = (rand - 0.75) / 0.20;
    return static_cast<uint32_t>(p75 + t * (p95 - p75));
  } else if (rand < 0.99) {
    // 95-99%: linear interpolation between p95 and p99
    double t = (rand - 0.95) / 0.04;
    return static_cast<uint32_t>(p95 + t * (p99 - p95));
  } else {
    // 99-100%: values above p99 (1.2x to 1.5x p99)
    double t = (rand - 0.99) / 0.01;
    return static_cast<uint32_t>(p99 * (1.0 + 0.5 * t));
  }
}

} // namespace ucachebench
} // namespace facebook
