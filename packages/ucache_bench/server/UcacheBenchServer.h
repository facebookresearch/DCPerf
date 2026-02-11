/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cachelib/allocator/CacheAllocator.h>
#include <folly/futures/Future.h>
#include <memory>
#include <string>
#ifdef OSS_BUILD
#include "UcacheBenchMessages.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchMessages.h"
#endif

namespace facebook {
namespace ucachebench {

using CacheAllocator = facebook::cachelib::Lru5B2QAllocator;
using PoolId = facebook::cachelib::PoolId;

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

  // Request handlers using Carbon protocol
  folly::SemiFuture<UcbGetReply> processUcbGet(const UcbGetRequest& req);
  folly::SemiFuture<UcbSetReply> processUcbSet(const UcbSetRequest& req);
  folly::SemiFuture<UcbDeleteReply> processUcbDelete(
      const UcbDeleteRequest& req);

  void printStats();

 private:
  void setupCacheLib();

  UcacheBenchConfig config_;
  std::unique_ptr<CacheAllocator> cache_;
  PoolId poolId_{};
};

} // namespace ucachebench
} // namespace facebook
