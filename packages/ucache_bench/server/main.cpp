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

#include "UcacheBenchOnRequest.h"
#include "UcacheBenchRpcServer.h"
#include "UcacheBenchServer.h"
#ifdef OSS_BUILD
#include "UcacheBenchServerOnRequestThrift.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchServerOnRequestThrift.h"
#endif

DEFINE_uint32(port, 11212, "Port to listen on");
DEFINE_bool(verbose, false, "Enable verbose logging");

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

void signal_handler(int sig) {
  fprintf(stderr, "Received signal %d, shutting down\n", sig);
  shutdown_requested = true;
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

std::unique_ptr<UcacheBenchRpcServer> makeAndStartUcacheBenchRpcServer() {
  // Create configuration from flags
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

  // Create the benchmark server with configuration
  auto server = std::make_shared<UcacheBenchServer>(config);

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

  if (FLAGS_verbose) {
    printf("Starting UcacheBench server on port %u\n", FLAGS_port);
  }

  try {
    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer starting\n");
    }
    auto ucacheBenchRpcServer = makeAndStartUcacheBenchRpcServer();
    if (FLAGS_verbose) {
      printf("UcacheBenchRpcServer started\n");
    }

    // Wait for shutdown signal
    folly::EventBase* evb = folly::EventBaseManager::get()->getEventBase();
    while (!shutdown_requested) {
      evb->loopOnce();
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
