/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchClient.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#include <folly/Random.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Promise.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GFlags.h>
#include <mcrouter/McrouterFiberContext.h>
#include <mcrouter/lib/network/CpuController.h>

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
    1000,
    "Connection timeout in milliseconds");
DEFINE_uint32(send_timeout_ms, 1000, "Send timeout in milliseconds");
DEFINE_string(
    security_mech,
    "plain",
    "Security mechanism for mcrouter (plain, tls_to_plain, fizz, etc.)");
DEFINE_uint32(
    num_proxies,
    0,
    "Number of mcrouter proxy threads (0 = auto-detect using hardware_concurrency)");
DEFINE_uint32(
    max_inflight,
    1,
    "Maximum number of concurrent in-flight requests (higher = better throughput, requires more memory)");
DEFINE_uint32(
    num_threads,
    0,
    "Number of client worker threads for request generation (0 = auto-detect, recommended: 4-16)");
DEFINE_uint32(
    additional_fanout,
    0,
    "Number of additional connections per server for fanout (0 = disabled, must be <= 32768 - num_proxies)");
DEFINE_bool(
    enable_random_source_ip,
    false,
    "Enable random source IP addresses for connection fanout (works with BucketHashSelector)");
DEFINE_bool(verbose, false, "Enable verbose logging");
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

  // Configure for Thrift transport to ucache server
  // Build pool config with optional additional_fanout for high connection count
  std::string poolConfig;
  if (FLAGS_additional_fanout > 0) {
    poolConfig = folly::sformat(
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
        FLAGS_additional_fanout);
  } else {
    poolConfig = folly::sformat(
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
  // This allows mcrouter to distribute work across multiple proxy threads
  if (FLAGS_num_proxies == 0) {
    // Auto-detect using hardware concurrency
    options.num_proxies = std::thread::hardware_concurrency();
  } else {
    options.num_proxies = FLAGS_num_proxies;
  }

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
    if (FLAGS_additional_fanout > 0) {
      uint32_t totalConnections = FLAGS_additional_fanout + options.num_proxies;
      printf(
          "  Connection fanout enabled: %u additional connections (total: %u)\n",
          FLAGS_additional_fanout,
          totalConnections);
    } else {
      printf("  Connection fanout disabled (using default connections)\n");
    }
    if (FLAGS_enable_random_source_ip) {
      printf(
          "  Random source IP enabled: requests will use random source IPs for additional fanout\n");
    }
    fflush(stdout);
  }
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

  uint32_t maxInflight = FLAGS_max_inflight;
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

  auto startTime = std::chrono::steady_clock::now();
  auto endTime = startTime + std::chrono::seconds(FLAGS_warmup_seconds);

  // Thread-safe counters
  std::atomic<uint64_t> totalOps{0};
  std::atomic<uint64_t> setSuccesses{0};
  std::atomic<uint64_t> setErrors{0};
  std::atomic<bool> shouldStop{false};

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
            "Warmup progress: %.1fs elapsed, %lu ops (%.1f QPS avg), Success: %.1f%%, Errors: %lu\n",
            elapsed,
            ops,
            avgQps,
            successRate,
            errors);
        fflush(stdout);
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

  // Worker coroutine - matches production LoadgenWorker::co_run() pattern
  auto warmupWorker =
      [&](memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>*
              clientPtr) -> folly::coro::Task<void> {
    folly::coro::AsyncScope scope;
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
      request.key_ref() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
      request.value_ref() = *folly::IOBuf::copyBuffer(value);
      request.exptime_ref() = 3600;

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::coro::makePromiseContract<UcbSetReply>();

      clientPtr->send(
          request,
          [p = std::move(promise)](
              const UcbSetRequest&, UcbSetReply&& reply) mutable {
            p.setValue(std::move(reply));
          });

      UcbSetReply result = co_await std::move(future);

      localOps++;
      if (*result.result_ref() == carbon::Result::STORED) {
        localSuccesses++;
      } else {
        localErrors++;
      }
      co_return;
    };

    // Main loop - matches production LoadgenWorker::co_run()
    while (std::chrono::steady_clock::now() < endTime) {
      // Spawn requests up to max_inflight
      size_t n = maxInflight - inflight.load();
      if (n > 0 && std::chrono::steady_clock::now() < endTime) {
        for (size_t i = 0; i < n && inflight.load() < maxInflight; i++) {
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

    co_await scope.joinAsync();

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

  if (FLAGS_verbose) {
    printf(
        "Starting benchmark for %u seconds with %u worker threads, max_inflight=%u per client\n",
        FLAGS_duration_seconds,
        numThreads,
        maxInflight);
    fflush(stdout);
  }

  auto startTime = std::chrono::steady_clock::now();
  auto endTime = startTime + std::chrono::seconds(FLAGS_duration_seconds);

  std::atomic<bool> shouldStop{false};

  // Mutex for latency vector (std::vector is not thread-safe)
  std::mutex latenciesMutex;
  std::vector<double> allLatencies;
  allLatencies.reserve(100000 * numThreads);

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

  // Worker coroutine - matches production LoadgenWorker::co_run() pattern
  auto benchmarkWorker =
      [&](size_t workerId,
          memcache::mcrouter::CarbonRouterClient<UcacheBenchRouterInfo>*
              clientPtr) -> folly::coro::Task<void> {
    folly::coro::AsyncScope scope;
    auto exe = co_await folly::coro::co_current_executor;

    // Send one GET request - matches production McrouterAdapter::coro() pattern
    auto sendGetRequest = [&]() -> folly::coro::Task<void> {
      auto opStartTime = std::chrono::steady_clock::now();
      std::string key = generateKey();

      UcbGetRequest request;
      request.key_ref() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::coro::makePromiseContract<UcbGetReply>();

      clientPtr->send(
          request,
          [p = std::move(promise)](
              const UcbGetRequest&, UcbGetReply&& reply) mutable {
            p.setValue(std::move(reply));
          });

      UcbGetReply result = co_await std::move(future);

      auto opEndTime = std::chrono::steady_clock::now();
      auto latencyMs =
          std::chrono::duration<double, std::milli>(opEndTime - opStartTime)
              .count();

      {
        std::lock_guard<std::mutex> lock(workerLatencyMutexes[workerId]);
        workerLatencies[workerId].push_back(latencyMs);
      }

      workerTotalOps[workerId]->fetch_add(1);
      workerGetOps[workerId]->fetch_add(1);

      if (*result.result_ref() == carbon::Result::FOUND) {
        workerGetHits[workerId]->fetch_add(1);
      } else if (*result.result_ref() == carbon::Result::NOTFOUND) {
        workerGetMisses[workerId]->fetch_add(1);

        // SET on GET miss to simulate real cache warming behavior
        // This matches the behavior from the old synchronous version
        std::string value = generateValue();

        UcbSetRequest setRequest;
        setRequest.key_ref() = carbon::Keys<folly::IOBuf>(
            std::move(*folly::IOBuf::copyBuffer(key)));
        setRequest.value_ref() = *folly::IOBuf::copyBuffer(value);
        setRequest.exptime_ref() = 3600;

        if (FLAGS_enable_random_source_ip) {
          uint8_t randomOctet = folly::Random::rand32(1, 255);
          std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
          try {
            folly::IPAddress sourceIp(ipStr);
            setRequest.setSourceIpAddr(sourceIp);
          } catch (const std::exception&) {
          }
        }

        auto [setPromise, setFuture] =
            folly::coro::makePromiseContract<UcbSetReply>();

        clientPtr->send(
            setRequest,
            [p = std::move(setPromise)](
                const UcbSetRequest&, UcbSetReply&& reply) mutable {
              p.setValue(std::move(reply));
            });

        UcbSetReply setResult = co_await std::move(setFuture);

        workerSetOps[workerId]->fetch_add(1);
        if (*setResult.result_ref() == carbon::Result::STORED) {
          workerSetSuccesses[workerId]->fetch_add(1);
        } else {
          workerSetErrors[workerId]->fetch_add(1);
        }
      } else {
        workerGetErrors[workerId]->fetch_add(1);
      }

      co_return;
    };

    // Send one SET request - matches production McrouterAdapter::coro() pattern
    auto sendSetRequest = [&]() -> folly::coro::Task<void> {
      auto opStartTime = std::chrono::steady_clock::now();
      std::string key = generateKey();
      std::string value = generateValue();

      UcbSetRequest request;
      request.key_ref() =
          carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
      request.value_ref() = *folly::IOBuf::copyBuffer(value);
      request.exptime_ref() = 3600;

      if (FLAGS_enable_random_source_ip) {
        uint8_t randomOctet = folly::Random::rand32(1, 255);
        std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
        try {
          folly::IPAddress sourceIp(ipStr);
          request.setSourceIpAddr(sourceIp);
        } catch (const std::exception&) {
        }
      }

      // Same pattern as production McrouterAdapter::coro()
      auto [promise, future] = folly::coro::makePromiseContract<UcbSetReply>();

      clientPtr->send(
          request,
          [p = std::move(promise)](
              const UcbSetRequest&, UcbSetReply&& reply) mutable {
            p.setValue(std::move(reply));
          });

      UcbSetReply result = co_await std::move(future);

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

      if (*result.result_ref() == carbon::Result::STORED) {
        workerSetSuccesses[workerId]->fetch_add(1);
      } else {
        workerSetErrors[workerId]->fetch_add(1);
      }

      co_return;
    };

    // Main loop - continuously spawn requests
    // McRouter's maximumOutstanding handles backpressure - it will block
    // when too many requests are in flight
    while (std::chrono::steady_clock::now() < endTime) {
      // Decide GET vs SET based on ratio
      bool isGet = (folly::Random::randDouble01() < getRatio);
      if (isGet) {
        scope.add(
            folly::coro::co_withExecutor(
                exe, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
                  co_await sendGetRequest();
                  co_return;
                })));
      } else {
        scope.add(
            folly::coro::co_withExecutor(
                exe, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
                  co_await sendSetRequest();
                  co_return;
                })));
      }

      // Yield to allow other coroutines to run
      co_await folly::coro::co_reschedule_on_current_executor;
    }

    co_await scope.joinAsync();

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

  // Notify admin server that benchmark is done (if connected)
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
    std::string baseKey = folly::sformat("key_{:08d}", keyId);

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
  return folly::sformat("key_{:08d}", keyId);
}

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

    std::string value;
    value.reserve(valueSize);

    // Generate random value content
    for (uint32_t i = 0; i < valueSize; ++i) {
      value.push_back('a' + (folly::Random::rand32(26)));
    }

    return value;
  }

  // Default behavior: random size between min and max
  uint32_t valueSize =
      folly::Random::rand32(FLAGS_value_size_min, FLAGS_value_size_max + 1);

  std::string value;
  value.reserve(valueSize);

  // Generate random value content
  for (uint32_t i = 0; i < valueSize; ++i) {
    value.push_back('a' + (folly::Random::rand32(26)));
  }

  return value;
}

// mcrouter operations using UcacheBench service
void UcacheBenchClient::sendUcbGetRequestSync(
    facebook::memcache::mcrouter::CarbonRouterClient<
        UcacheBenchRouterInfo>::Pointer& client,
    const std::string& key,
    const std::function<void(UcbGetReply&&)>& callback) {
  UcbGetRequest request;
  request.key_ref() =
      carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));

  // Set random source IP if enabled for connection fanout
  if (FLAGS_enable_random_source_ip) {
    // Generate a random IPv6 address in the format ::ffff:192.0.2.X
    // Using IPv4-mapped IPv6 addresses for simplicity
    uint8_t randomOctet = folly::Random::rand32(1, 255);
    std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
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
    result.message_ref() = "Failed to send GET request to mcrouter";
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
  request.key_ref() =
      carbon::Keys<folly::IOBuf>(std::move(*folly::IOBuf::copyBuffer(key)));
  request.value_ref() = *folly::IOBuf::copyBuffer(value);
  request.exptime_ref() = 3600; // 1 hour default TTL

  // Set random source IP if enabled for connection fanout
  if (FLAGS_enable_random_source_ip) {
    // Generate a random IPv6 address in the format ::ffff:192.0.2.X
    // Using IPv4-mapped IPv6 addresses for simplicity
    uint8_t randomOctet = folly::Random::rand32(1, 255);
    std::string ipStr = folly::sformat("::ffff:192.0.2.{}", randomOctet);
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
    result.message_ref() = "Failed to send SET request to mcrouter";
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
        folly::sformat("Failed to open distribution config: {}", configFile));
  }

  std::string content(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // Parse JSON using folly::dynamic
  folly::dynamic config;
  try {
    config = folly::parseJson(content);
  } catch (const std::exception& e) {
    throw std::runtime_error(
        folly::sformat("Failed to parse JSON config: {}", e.what()));
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
