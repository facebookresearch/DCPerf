/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/Init.h>
#include <folly/portability/GFlags.h>
#include <signal.h>
#include <memory>

#include "UcacheBenchAdminServer.h"
#include "UcacheBenchOnRequest.h"
#include "UcacheBenchRpcServer.h"
#include "UcacheBenchServer.h"
#ifdef OSS_BUILD
#include "UcacheBenchServerOnRequestThrift.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchServerOnRequestThrift.h"
#endif

DEFINE_uint32(port, 11212, "Port to listen on");

// Admin server flags for multi-client coordination
DEFINE_int32(
    admin_port,
    -1,
    "Admin port for multi-client coordination (-1 = auto, uses port+1 when num_clients > 0; 0 = disabled)");
DEFINE_uint32(
    num_clients,
    0,
    "Number of clients expected to connect (enables admin server when > 0)");
DEFINE_uint32(
    timeout_seconds,
    600,
    "Timeout in seconds for waiting for clients (0 = no timeout)");
DEFINE_bool(verbose, false, "Enable verbose logging");
DEFINE_uint32(
    stats_interval_seconds,
    10,
    "Periodic stats reporting interval in seconds during warmup/benchmark (0 = disable)");

// CacheLib configuration flags
DEFINE_uint64(
    memory_mb,
    1024,
    "Memory size in MB for DRAM cache (always used)");
DEFINE_uint32(
    hash_power,
    20,
    "Hash table power for cachelib (number of hash buckets = 2^hash_power)");
DEFINE_string(pool_name, "default", "Pool name for cachelib");

// LRU rebalancing configuration
DEFINE_uint32(
    lru_rebalance_interval_sec,
    0,
    "LRU rebalance interval in seconds (0 = disabled)");
DEFINE_uint32(
    lru_rebalancing_hits_min_age_sec,
    0,
    "Minimum LRU tail age in seconds to reduce slabs");
DEFINE_uint32(
    lru_rebalancing_hits_max_age_sec,
    0,
    "Maximum LRU tail age in seconds to increase slabs");
DEFINE_bool(
    lru_hits_victim_by_free_mem,
    false,
    "Use free memory for LRU rebalancing victim selection");

// Hash table lock configuration
DEFINE_uint32(
    hashtable_lock_power,
    20,
    "Hash table lock power (number of locks = 2^lock_power)");

// CacheLib allocator settings
DEFINE_uint64(
    cachelib_num_shards,
    0,
    "Number of CacheLib shards (0 = use default)");
DEFINE_uint32(min_alloc_size, 64, "Minimum allocation size in bytes");

// Navy (NVM/SSD cache) configuration (if navy_cache_size_mb > 0, hybrid mode
// is enabled)
DEFINE_string(
    navy_cache_path,
    "/tmp/ucachebench_ssd",
    "Path for Navy cache files");
DEFINE_uint64(
    navy_cache_size_mb,
    0,
    "Navy cache size in MB (0 = DRAM-only, >0 = hybrid DRAM+Navy mode)");
DEFINE_uint32(navy_block_size, 4096, "Navy block size in bytes");
DEFINE_uint32(
    navy_device_max_write_rate,
    0,
    "Max Navy write rate MB/s (0 = unlimited)");
DEFINE_uint32(navy_region_size_mb, 16, "Navy region size in MB");
DEFINE_uint32(
    navy_clean_regions_pool,
    4,
    "Number of clean regions to maintain");
DEFINE_bool(navy_truncate_file, true, "Truncate Navy cache file on startup");

// Navy advanced configuration flags
DEFINE_uint32(
    navy_reader_threads,
    32,
    "Number of Navy reader threads (default: 32)");
DEFINE_uint32(
    navy_writer_threads,
    32,
    "Number of Navy writer threads (default: 32)");
DEFINE_uint32(
    navy_bighash_size_pct,
    50,
    "Percentage of Navy space for BigHash (small objects), 0 to disable (default: 50)");
DEFINE_uint32(
    navy_bighash_max_item_size,
    2048,
    "Max item size for BigHash in bytes (default: 2048)");
DEFINE_uint32(
    navy_bighash_bucket_size,
    4096,
    "BigHash bucket size in bytes (default: 4096)");
DEFINE_uint32(
    navy_max_concurrent_inserts,
    1000000,
    "Max concurrent Navy inserts (default: 1000000)");
DEFINE_uint32(
    navy_max_parcel_memory_mb,
    256,
    "Max Navy parcel memory in MB (default: 256)");
DEFINE_uint64(
    navy_admission_write_rate_mb,
    0,
    "Navy admission write rate in MB/s (0 = disabled, for device endurance)");
DEFINE_uint32(
    navy_clean_region_threads,
    4,
    "Number of Navy clean region threads (default: 4)");
DEFINE_uint32(
    navy_metadata_size_mb,
    100,
    "Navy metadata size in MB (default: 100)");

using namespace facebook::ucachebench;

namespace {
bool shutdown_requested = false;
UcacheBenchAdminServer* g_adminServer = nullptr;

void signal_handler(int sig) {
  fprintf(stderr, "Received signal %d, shutting down\n", sig);
  shutdown_requested = true;

  // If admin server is running, request it to shutdown
  if (g_adminServer) {
    g_adminServer->requestShutdown();
  }
}

void setup_signal_handlers() {
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction(SIGINT, &sa, nullptr) != 0) {
    perror("Failed to set up SIGINT handler");
  }
  if (sigaction(SIGTERM, &sa, nullptr) != 0) {
    perror("Failed to set up SIGTERM handler");
  }
}

// Create configuration from command-line flags
UcacheBenchConfig createConfigFromFlags() {
  UcacheBenchConfig config;

  // Basic settings
  config.memory_mb = FLAGS_memory_mb;
  config.hash_power = FLAGS_hash_power;
  config.pool_name = FLAGS_pool_name;
  config.verbose = FLAGS_verbose;

  // LRU rebalancing settings
  config.lru_rebalance_interval_sec = FLAGS_lru_rebalance_interval_sec;
  config.lru_rebalancing_hits_min_age_sec =
      FLAGS_lru_rebalancing_hits_min_age_sec;
  config.lru_rebalancing_hits_max_age_sec =
      FLAGS_lru_rebalancing_hits_max_age_sec;
  config.lru_hits_victim_by_free_mem = FLAGS_lru_hits_victim_by_free_mem;

  // Hash table settings
  config.hashtable_lock_power = FLAGS_hashtable_lock_power;

  // CacheLib allocator settings
  config.cachelib_num_shards = FLAGS_cachelib_num_shards;
  config.min_alloc_size = FLAGS_min_alloc_size;

  // Navy settings
  config.navy_cache_path = FLAGS_navy_cache_path;
  config.navy_cache_size_mb = FLAGS_navy_cache_size_mb;
  config.navy_block_size = FLAGS_navy_block_size;
  config.navy_device_max_write_rate = FLAGS_navy_device_max_write_rate;
  config.navy_region_size_mb = FLAGS_navy_region_size_mb;
  config.navy_clean_regions_pool = FLAGS_navy_clean_regions_pool;
  config.navy_truncate_file = FLAGS_navy_truncate_file;
  config.navy_reader_threads = FLAGS_navy_reader_threads;
  config.navy_writer_threads = FLAGS_navy_writer_threads;
  config.navy_bighash_size_pct = FLAGS_navy_bighash_size_pct;
  config.navy_bighash_max_item_size = FLAGS_navy_bighash_max_item_size;
  config.navy_bighash_bucket_size = FLAGS_navy_bighash_bucket_size;
  config.navy_max_concurrent_inserts = FLAGS_navy_max_concurrent_inserts;
  config.navy_max_parcel_memory_mb = FLAGS_navy_max_parcel_memory_mb;
  config.navy_admission_write_rate_mb = FLAGS_navy_admission_write_rate_mb;
  config.navy_clean_region_threads = FLAGS_navy_clean_region_threads;
  config.navy_metadata_size_mb = FLAGS_navy_metadata_size_mb;

  return config;
}

std::unique_ptr<UcacheBenchRpcServer> makeAndStartUcacheBenchRpcServer(
    std::shared_ptr<UcacheBenchServer> server) {
  if (FLAGS_verbose) {
    printf("Server initialized successfully. Starting server...\n");
    server->printStats(); // Print initial cache stats
  }

  auto ucacheBenchRpcServer = std::make_unique<UcacheBenchRpcServer>();

  auto& thriftServer = ucacheBenchRpcServer->addThriftServer();
  thriftServer.setPort(FLAGS_port);

  // Create a map of EventBase to OnRequest handlers (similar to production
  // ucache)
  std::unordered_map<folly::EventBase*, std::shared_ptr<UcacheBenchOnRequest>>
      serverOnRequestMap;

  if (FLAGS_verbose) {
    printf("Thrift server listening on port %u\n", FLAGS_port);
    printf("Press Ctrl+C to shutdown.\n");
  }

  ucacheBenchRpcServer->start(
      [server, &serverOnRequestMap](folly::EventBase& evb) {
        // Thread initialization - create OnRequest handler for this EventBase
        folly::setThreadName("ucachebench_worker");
        auto onRequest = std::make_shared<UcacheBenchOnRequest>(server);
        serverOnRequestMap[&evb] = onRequest;
      },
      []() {
        // Thread cleanup - can be extended as needed
      });

  // Create and set the Thrift handler using the serverOnRequestMap
  auto thriftHandler =
      std::make_shared<UcacheBenchServerOnRequestThrift<UcacheBenchOnRequest>>(
          serverOnRequestMap);
  thriftServer.setInterface(thriftHandler);

  // Now that the interface is set, start serving
  ucacheBenchRpcServer->serve();

  return ucacheBenchRpcServer;
}

} // namespace

int main(int argc, char** argv) {
  folly::init(&argc, &argv, true);

  // Initialize logging with reasonable defaults
  folly::initLogging("INFO");

  setup_signal_handlers();

  // Validate port flags
  if (FLAGS_port > 65535) {
    fprintf(
        stderr, "Error: --port must be less than 65536 (got %u)\n", FLAGS_port);
    return 1;
  }

  if (FLAGS_verbose) {
    printf("Starting UcacheBench server on port %u\n", FLAGS_port);
  }

  // Auto-compute admin_port if num_clients > 0 and admin_port not explicitly
  // set
  int32_t effectiveAdminPort = FLAGS_admin_port;
  if (FLAGS_num_clients > 0 && FLAGS_admin_port == -1) {
    effectiveAdminPort = static_cast<int32_t>(FLAGS_port) + 1;
    printf(
        "[Server] Multi-client mode enabled: admin_port auto-set to %d (port + 1)\n",
        effectiveAdminPort);
  } else if (FLAGS_admin_port == -1) {
    // No multi-client mode, disable admin server
    effectiveAdminPort = 0;
  }

  // Validate admin server flags
  if (effectiveAdminPort > 65535) {
    fprintf(
        stderr,
        "Error: --admin_port must be less than 65536 (got %d)\n",
        effectiveAdminPort);
    return 1;
  }
  if (effectiveAdminPort > 0 && FLAGS_num_clients == 0) {
    fprintf(
        stderr,
        "Error: --num_clients is required when --admin_port is specified\n");
    return 1;
  }

  try {
    // Create the cache server configuration and instance first
    auto config = createConfigFromFlags();
    auto server = std::make_shared<UcacheBenchServer>(config);

    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer starting\n");
    }
    auto ucacheBenchRpcServer = makeAndStartUcacheBenchRpcServer(server);
    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer started\n");
    }

    // Set up admin server for multi-client coordination if enabled
    std::unique_ptr<UcacheBenchAdminServer> adminServer;
    if (effectiveAdminPort > 0) {
      adminServer = std::make_unique<UcacheBenchAdminServer>(
          static_cast<uint16_t>(effectiveAdminPort),
          FLAGS_num_clients,
          FLAGS_timeout_seconds);

      // Set global pointer for signal handler
      g_adminServer = adminServer.get();

      // Set up phase change callback for metric tracking
      adminServer->setPhaseChangeCallback([server](
                                              UcacheBenchAdminServer::Phase
                                                  phase) {
        switch (phase) {
          case UcacheBenchAdminServer::Phase::WARMUP:
            printf(
                "[Main] Phase changed to WARMUP - starting warmup stats tracking\n");
            server->setTrackingPhase(UcacheBenchServer::TrackingPhase::WARMUP);
            break;
          case UcacheBenchAdminServer::Phase::BENCHMARK:
            printf(
                "[Main] Phase changed to BENCHMARK - starting benchmark stats tracking\n");
            server->setTrackingPhase(
                UcacheBenchServer::TrackingPhase::BENCHMARK);
            break;
          default:
            break;
        }
      });

      // Set up print results callback
      adminServer->setPrintResultsCallback([&adminServer, server]() {
        auto benchmarkStart = adminServer->getBenchmarkStartTime();
        auto benchmarkEnd = adminServer->getBenchmarkEndTime();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              benchmarkEnd - benchmarkStart)
                              .count();
        double durationSec = durationMs / 1000.0;

        // Print results from the cache server
        server->printFinalResults(durationSec);
      });

      adminServer->start();

      // Start periodic server-side stats reporting
      server->startPeriodicStats(FLAGS_stats_interval_seconds);
    }

    // If admin server is enabled, wait for it to complete
    // Otherwise, wait for shutdown signal
    if (adminServer) {
      bool completed = adminServer->waitForCompletion();
      if (!completed) {
        printf("Admin server timed out or failed\n");
      }
      // Stop periodic stats before printing final results
      server->stopPeriodicStats();
      // Stop the admin server
      adminServer->stop();
    } else {
      // Wait for shutdown signal
      folly::EventBase* evb = folly::EventBaseManager::get()->getEventBase();
      while (!shutdown_requested) {
        evb->loopOnce();
      }
    }

    if (FLAGS_verbose) {
      printf("Shutting down server...\n");
    }

    if (FLAGS_verbose) {
      printf("Ensuring UcacheBenchRpcServer has no new connections\n");
    }
    ucacheBenchRpcServer->ensureAcceptorsShutdown();

    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer stopping\n");
    }
    ucacheBenchRpcServer->stop();
    ucacheBenchRpcServer.reset();
    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer stopped\n");
    }

    return 0;
  } catch (const std::exception& ex) {
    printf("Server error: %s\n", ex.what());
    return 1;
  }
}
