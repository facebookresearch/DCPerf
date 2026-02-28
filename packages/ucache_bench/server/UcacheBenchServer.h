/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cachelib/allocator/CacheAllocator.h>
#include <folly/futures/Future.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#ifdef OSS_BUILD
#include "UcacheBenchMessages.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchMessages.h"
#endif

namespace facebook {
namespace ucachebench {

using CacheAllocator = facebook::cachelib::Lru5B2QAllocator;
using PoolId = facebook::cachelib::PoolId;

/**
 * Metrics tracking for a single benchmark phase (warmup or benchmark).
 * All counters are atomic for thread-safe updates from multiple IO threads.
 */
struct PhaseMetrics {
  std::atomic<uint64_t> getRequests{0};
  std::atomic<uint64_t> getHits{0};
  std::atomic<uint64_t> getMisses{0};
  std::atomic<uint64_t> setRequests{0};
  std::atomic<uint64_t> deleteRequests{0};

  void reset() {
    getRequests.store(0);
    getHits.store(0);
    getMisses.store(0);
    setRequests.store(0);
    deleteRequests.store(0);
  }
};

// CPU architecture enum for production-like cache configurations
// Each profile is tuned for specific CPU characteristics and memory capacity
enum class CpuArchitecture {
  DEFAULT, // Basic config without production tuning
  TURIN, // AMD Turin/Trento - high core count, 256GB+ RAM
  SKYLAKE, // Intel Skylake - 64GB RAM
  SAPPHIRE_RAPIDS, // Intel Sapphire Rapids - 128GB RAM
};

// Convert string to CpuArchitecture
CpuArchitecture parseCpuArchitecture(const std::string& str);

// Get string representation of CpuArchitecture
const char* cpuArchitectureToString(CpuArchitecture type);

struct UcacheBenchConfig {
  // Basic cache settings
  uint64_t memory_mb = 1024;
  uint32_t hash_power = 20;
  std::string pool_name = "default";
  bool verbose = false;

  // LRU rebalancing metric type for DRAM-only vs hybrid mode
  enum class LruRebalanceMetric {
    FAIR, // LruTailAgeStrategy - balances eviction across allocation classes
    HITS, // HitsPerSlabStrategy - optimizes for hit rate
  };

  // LRU rebalancing strategy selection
  // FAIR (LruTailAgeStrategy): Used in DRAM-only mode to balance large/small
  // item eviction HITS (HitsPerSlabStrategy): Used when NVM is enabled
  LruRebalanceMetric lru_rebalance_metric = LruRebalanceMetric::FAIR;

  // LRU rebalancing settings (common)
  uint32_t lru_rebalance_interval_sec = 0; // 0 = disabled
  uint32_t lru_rebalancing_min_slabs = 1; // Min slabs before rebalancing

  // FAIR strategy settings (LruTailAgeStrategy for DRAM-only mode)
  // Balances eviction by ensuring similar tail ages across allocation classes
  uint32_t lru_rebalancing_fair_min_diff =
      1200; // Min tail age difference threshold (seconds)
  float lru_rebalancing_fair_ratio =
      0.0f; // Tail age difference ratio (0 = disabled)

  // HITS strategy settings (HitsPerSlabStrategy for hybrid mode with NVM)
  uint32_t lru_rebalancing_hits_min_age_sec = 0; // Min tail age to reduce slabs
  uint32_t lru_rebalancing_hits_max_age_sec =
      7200; // Max tail age to increase slabs
  bool lru_hits_victim_by_free_mem = false;

  // Hash table settings for access config
  uint32_t hashtable_lock_power = 20; // Number of locks = 2^lock_power

  // CacheLib allocator settings
  uint64_t cachelib_num_shards = 0; // 0 = use default
  uint32_t min_alloc_size = 64; // Minimum allocation size in bytes

  // Navy (NVM/SSD cache) config (if navy_cache_size_mb > 0, hybrid mode is
  // enabled)
  std::string navy_cache_path = "/tmp/ucachebench_ssd";
  uint64_t navy_cache_size_mb = 0;
  uint32_t navy_block_size = 4096;
  uint32_t navy_device_max_write_rate = 0;
  uint32_t navy_region_size_mb = 16;
  uint32_t navy_clean_regions_pool = 4;
  bool navy_truncate_file = true;
  // Navy advanced configurations
  uint32_t navy_reader_threads = 32;
  uint32_t navy_writer_threads = 32;
  uint32_t navy_bighash_size_pct = 50; // 50% for BigHash, 50% for BlockCache
  uint32_t navy_bighash_max_item_size = 2048; // 2KB items go to BigHash
  uint32_t navy_bighash_bucket_size = 4096;
  uint32_t navy_max_concurrent_inserts = 1000000;
  uint32_t navy_max_parcel_memory_mb = 256;
  uint64_t navy_admission_write_rate_mb = 0; // 0 = disabled
  uint32_t navy_clean_region_threads = 4;
  uint32_t navy_metadata_size_mb = 100;
};

class UcacheBenchServer {
 public:
  explicit UcacheBenchServer(const UcacheBenchConfig& config);
  ~UcacheBenchServer();

  // Request handlers using Carbon protocol
  folly::SemiFuture<UcbGetReply> processUcbGet(const UcbGetRequest& req);
  folly::SemiFuture<UcbSetReply> processUcbSet(const UcbSetRequest& req);
  folly::SemiFuture<UcbDeleteReply> processUcbDelete(
      const UcbDeleteRequest& req);

  void printStats();

  // Phase-based metric tracking for multi-client coordination
  enum class TrackingPhase {
    NONE, // Not tracking (before warmup starts)
    WARMUP, // Tracking warmup phase
    BENCHMARK // Tracking benchmark phase
  };

  // Set the current tracking phase and reset corresponding metrics
  void setTrackingPhase(TrackingPhase phase);

  // Get current tracking phase
  TrackingPhase getTrackingPhase() const {
    return currentPhase_.load();
  }

  // Get metrics for warmup phase
  const PhaseMetrics& getWarmupMetrics() const {
    return warmupMetrics_;
  }

  // Get metrics for benchmark phase
  const PhaseMetrics& getBenchmarkMetrics() const {
    return benchmarkMetrics_;
  }

  // Print final results in parseable format for benchpress
  void printFinalResults(double benchmarkDurationSec) const;

  // Periodic stats reporting
  void startPeriodicStats(uint32_t intervalSec);
  void stopPeriodicStats();

 private:
  void setupCacheLib();

  // Increment metrics based on current phase
  void recordGet(bool hit);
  void recordSet();
  void recordDelete();

  // Periodic stats thread function
  void periodicStatsLoop(uint32_t intervalSec);

  UcacheBenchConfig config_;
  std::unique_ptr<CacheAllocator> cache_;
  PoolId poolId_{};

  // Phase-based metric tracking
  std::atomic<TrackingPhase> currentPhase_{TrackingPhase::NONE};
  PhaseMetrics warmupMetrics_;
  PhaseMetrics benchmarkMetrics_;

  // Periodic stats thread
  std::thread statsThread_;
  std::atomic<bool> statsRunning_{false};
  std::mutex statsMutex_;
  std::condition_variable statsCv_;
};

} // namespace ucachebench
} // namespace facebook
