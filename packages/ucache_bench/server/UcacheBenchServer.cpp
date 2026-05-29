/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchServer.h"

#include <folly/BenchmarkUtil.h>
#include <folly/Format.h>
#include <folly/hash/Hash.h>
#include <folly/portability/GFlags.h>
#include <chrono>
#include <time.h>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"
#include "cachelib/allocator/LruTailAgeStrategy.h"

using namespace facebook::ucachebench;

namespace facebook {
namespace ucachebench {

UcacheBenchServer::UcacheBenchServer(const UcacheBenchConfig& config)
    : config_(config) {
  setupCacheLib();

  // Initialize CacheTable-style bucket locks if enabled
  if (config_.bucket_lock_power > 0) {
    bucketLocks_ = std::make_unique<BucketLocks>(
        config_.bucket_lock_power,
        std::make_shared<facebook::cachelib::MurmurHash2>());
    if (config_.verbose) {
      printf(
          "Bucket locking enabled: 2^%u = %u locks (fiber-aware RW mutex)\n",
          config_.bucket_lock_power,
          1u << config_.bucket_lock_power);
    }
  }

  // Initialize production-like ACL prefix table
  // Simulates production's granular ACL prefix categories.
  // Production has ~100-500 ACL categories with prefix matching.
  if (config_.production_features_enabled) {
    for (uint64_t i = 0; i < 256; ++i) {
      aclPrefixTable_[i] = static_cast<uint32_t>(i % 8); // 8 ACL categories
    }
    if (config_.verbose) {
      printf(
          "Production features enabled: key construction, stats tracking, "
          "ACL checks, overload protection\n");
    }
  }
}

UcacheBenchServer::~UcacheBenchServer() {
  stopPeriodicStats();
}

void UcacheBenchServer::setupCacheLib() {
  // CacheLib always requires DRAM cache initialization
  // Navy (NVM/SSD cache) is optionally enabled based on navy_cache_size_mb
  bool enableNvm = (config_.navy_cache_size_mb > 0);

  CacheAllocator::Config cacheConfig;

  // Set memory size (L1 DRAM cache)
  cacheConfig.setCacheSize(config_.memory_mb * 1024 * 1024);

  // Configure allocator settings
  // hash_power: Number of hash buckets = 2^hash_power
  // lock_power: Number of locks = 2^lock_power
  cacheConfig.setAccessConfig(
      {config_.hash_power, config_.hashtable_lock_power});

  // Configure number of CacheLib shards if specified
  if (config_.cachelib_num_shards > 0) {
    cacheConfig.setNumShards(config_.cachelib_num_shards);
  }

  // Generate alloc sizes (factor 1.25, min allocation size)
  // This provides a good distribution of allocation classes for cache items
  // Max alloc size increased to 64KB to support production traffic distribution
  cacheConfig.setDefaultAllocSizes(
      facebook::cachelib::util::generateAllocSizes(
          1.25 /* alloc factor */,
          65536 /* max alloc size (64KB) */,
          config_.min_alloc_size /* min alloc size */));

  // Configure LRU rebalancing if enabled
  if (config_.lru_rebalance_interval_sec > 0) {
    std::shared_ptr<facebook::cachelib::RebalanceStrategy> rebalanceStrategy;

    if (!enableNvm &&
        config_.lru_rebalance_metric ==
            UcacheBenchConfig::LruRebalanceMetric::FAIR) {
      // DRAM-only mode: Use LruTailAgeStrategy (FAIR) to balance large/small
      // item eviction. This is the production ucache OCI setting for DRAM-only
      // deployments. The FAIR strategy ensures that allocation classes with
      // different item sizes have similar tail ages, preventing one size class
      // from dominating eviction.
      facebook::cachelib::LruTailAgeStrategy::Config fairConfig;
      fairConfig.minSlabs = config_.lru_rebalancing_min_slabs;
      fairConfig.tailAgeDifferenceRatio = config_.lru_rebalancing_fair_ratio;
      fairConfig.minTailAgeDifference = config_.lru_rebalancing_fair_min_diff;
      rebalanceStrategy =
          std::make_shared<facebook::cachelib::LruTailAgeStrategy>(fairConfig);

      if (config_.verbose) {
        printf(
            "  LRU rebalancing strategy: FAIR (LruTailAgeStrategy)\n"
            "    minSlabs=%u, tailAgeDifferenceRatio=%.2f, minTailAgeDifference=%u\n",
            config_.lru_rebalancing_min_slabs,
            config_.lru_rebalancing_fair_ratio,
            config_.lru_rebalancing_fair_min_diff);
      }
    } else {
      // Hybrid mode (NVM enabled) or HITS metric: Use HitsPerSlabStrategy
      // This is the production ucache setting when NVM is enabled.
      facebook::cachelib::HitsPerSlabStrategy::Config hitsConfig;
      hitsConfig.minSlabs = config_.lru_rebalancing_min_slabs;
      hitsConfig.minLruTailAge = config_.lru_rebalancing_hits_min_age_sec;
      hitsConfig.maxLruTailAge = config_.lru_rebalancing_hits_max_age_sec;
      rebalanceStrategy =
          std::make_shared<facebook::cachelib::HitsPerSlabStrategy>(hitsConfig);

      if (config_.verbose) {
        printf(
            "  LRU rebalancing strategy: HITS (HitsPerSlabStrategy)\n"
            "    minSlabs=%u, minLruTailAge=%u, maxLruTailAge=%u\n",
            config_.lru_rebalancing_min_slabs,
            config_.lru_rebalancing_hits_min_age_sec,
            config_.lru_rebalancing_hits_max_age_sec);
      }
    }
    cacheConfig.enablePoolRebalancing(
        rebalanceStrategy,
        std::chrono::seconds(config_.lru_rebalance_interval_sec));
  }

  if (config_.verbose) {
    printf("CacheLib configuration:\n");
    printf(
        "  hash_power=%u, lock_power=%u\n",
        config_.hash_power,
        config_.hashtable_lock_power);
    if (config_.lru_rebalance_interval_sec > 0) {
      printf(
          "  lru_rebalance_interval=%us, hits_min_age=%us, hits_max_age=%us\n",
          config_.lru_rebalance_interval_sec,
          config_.lru_rebalancing_hits_min_age_sec,
          config_.lru_rebalancing_hits_max_age_sec);
    }
    if (config_.cachelib_num_shards > 0) {
      printf("  cachelib_num_shards=%lu\n", config_.cachelib_num_shards);
    }

    if (enableNvm) {
      printf(
          "Initializing CacheLib in HYBRID mode with %luMB RAM + %luMB Navy\n",
          config_.memory_mb,
          config_.navy_cache_size_mb);
    } else {
      printf(
          "Initializing CacheLib with %luMB DRAM memory\n", config_.memory_mb);
    }
  }

  // Optionally configure Navy (NVM/SSD cache) if navy_cache_size_mb > 0
  if (enableNvm) {
    CacheAllocator::NvmCacheConfig nvmConfig;

    // Set Navy reader/writer threads (similar to production ucache)
    nvmConfig.navyConfig.setReaderAndWriterThreads(
        config_.navy_reader_threads, config_.navy_writer_threads);

    // Set Navy request ordering shards
    nvmConfig.navyConfig.setNavyReqOrderingShards(10);

    // Set max concurrent inserts
    nvmConfig.navyConfig.setMaxConcurrentInserts(
        config_.navy_max_concurrent_inserts);

    // Set max parcel memory
    nvmConfig.navyConfig.setMaxParcelMemoryMB(
        config_.navy_max_parcel_memory_mb);

    // Configure BigHash for small objects (similar to production)
    if (config_.navy_bighash_size_pct > 0) {
      nvmConfig.navyConfig.bigHash()
          .setSizePctAndMaxItemSize(
              config_.navy_bighash_size_pct, config_.navy_bighash_max_item_size)
          .setBucketSize(config_.navy_bighash_bucket_size);
    }

    // Set block size
    nvmConfig.navyConfig.setBlockSize(config_.navy_block_size);

    // Set metadata size
    nvmConfig.navyConfig.setDeviceMetadataSize(
        config_.navy_metadata_size_mb * 1024 * 1024);

    // Enable admission policy if configured (for SSD endurance)
    if (config_.navy_admission_write_rate_mb > 0) {
      nvmConfig.navyConfig.enableDynamicRandomAdmPolicy()
          .setAdmWriteRate(config_.navy_admission_write_rate_mb * 1024 * 1024)
          .setMaxWriteRate(
              config_.navy_admission_write_rate_mb * 1024 * 1024 * 2);
    }

    // Configure BlockCache regions (for larger objects)
    nvmConfig.navyConfig.blockCache()
        .setRegionSize(config_.navy_region_size_mb * 1024 * 1024)
        .setCleanRegions(
            config_.navy_clean_regions_pool, config_.navy_clean_region_threads);

    // Set device max write size
    if (config_.navy_device_max_write_rate > 0) {
      nvmConfig.navyConfig.setDeviceMaxWriteSize(
          config_.navy_device_max_write_rate * 1024 * 1024);
    }

    // Configure the Navy file/device
    nvmConfig.navyConfig.setSimpleFile(
        config_.navy_cache_path,
        config_.navy_cache_size_mb * 1024 * 1024,
        config_.navy_truncate_file);

    // Enable NvmCache in the config
    cacheConfig.enableNvmCache(nvmConfig);

    if (config_.verbose) {
      printf("  Navy cache path: %s\n", config_.navy_cache_path.c_str());
      printf(
          "  Navy threads: %u readers, %u writers\n",
          config_.navy_reader_threads,
          config_.navy_writer_threads);
      printf(
          "  BigHash: %u%% of space, max item size %uB\n",
          config_.navy_bighash_size_pct,
          config_.navy_bighash_max_item_size);
      printf(
          "  BlockCache: %uMB regions, %u clean regions\n",
          config_.navy_region_size_mb,
          config_.navy_clean_regions_pool);
    }
  }

  cache_ = std::make_unique<CacheAllocator>(std::move(cacheConfig));

  // Create default pool using actual usable memory after CacheLib overhead
  // (hash table, slab headers, Navy metadata, etc.)
  size_t usableMemory = cache_->getCacheMemoryStats().ramCacheSize;

  if (config_.verbose) {
    printf(
        "Usable memory after CacheLib overhead: %zu bytes (%.2f MB)\n",
        usableMemory,
        usableMemory / (1024.0 * 1024.0));
  }

  poolId_ = cache_->addPool(config_.pool_name, usableMemory);

  if (config_.verbose) {
    printf(
        "Hybrid cache initialized successfully with pool: %s\n",
        config_.pool_name.c_str());
  }
}

void UcacheBenchServer::simulatePerRequestOverhead(const std::string& key) {
  if (config_.cpu_overhead_level == 0) {
    return;
  }

  // Level 1+: Simulate ACL/identity hash checks and key construction
  // Production ucache computes identity hashes for access control, constructs
  // CacheTable keys with region/pool prefixes, and reads timestamps.
  uint64_t hash = folly::hash::fnv64(key);
  hash = folly::hash::twang_mix64(hash);
  hash = folly::hash::fnv64_buf(key.data(), key.size(), hash);

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  clock_gettime(CLOCK_REALTIME, &ts);

  // Level 1+: Simulate per-request memory allocations
  // Production ucache does string allocations for key construction,
  // serialization buffers, identity objects, and ACL results. These
  // allocations create jemalloc mutex contention under high concurrency,
  // which is a major contributor to production kernel CPU (~7.5% from
  // queued_spin_lock_slowpath in futex for jemalloc).
  {
    // Simulate key construction + identity string allocation
    auto buf = std::make_unique<char[]>(256);
    std::memcpy(buf.get(), key.data(), std::min(key.size(), size_t(256)));
    folly::doNotOptimizeAway(buf.get());
  }

  if (config_.cpu_overhead_level >= 2) {
    // Level 2+: More hash computation + larger allocations
    hash = folly::hash::fnv64_buf(key.data(), key.size(), hash);
    hash = folly::hash::twang_mix64(hash);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_REALTIME, &ts);

    // Simulate serialization buffer allocation (attribute serialization,
    // privacy logging buffer, Luna sampling context)
    {
      auto buf = std::make_unique<char[]>(512);
      std::memset(buf.get(), 0, 512);
      std::memcpy(buf.get(), key.data(), std::min(key.size(), size_t(512)));
      folly::doNotOptimizeAway(buf.get());
    }
  }

  if (config_.cpu_overhead_level >= 3) {
    // Level 3: Heavy allocation pressure matching production
    // Production creates multiple temporary objects per request:
    // identity context, ACL result, privacy log entry, sampling decision,
    // data access zone policy result, response attributes.
    for (int i = 0; i < 3; ++i) {
      hash = folly::hash::fnv64_buf(key.data(), key.size(), hash);
      hash = folly::hash::twang_mix64(hash);
      clock_gettime(CLOCK_MONOTONIC, &ts);

      auto buf = std::make_unique<char[]>(1024);
      std::memset(buf.get(), static_cast<int>(hash & 0xFF), 1024);
      folly::doNotOptimizeAway(buf.get());
    }
  }

  folly::doNotOptimizeAway(hash);
  folly::doNotOptimizeAway(ts);
}

// Build compound key matching production McStoredKey construction.
// Production builds: UcacheStoredKey(key, hashAlias, kcbId, ticket)
// This involves string concatenation, hashing, and memory allocation.
std::string UcacheBenchServer::buildCompoundKey(const std::string& key) {
  // Matches createMcStoredKeyFromRequest: prefix + key + kcbId suffix
  // Production allocates UcacheStoredKey with region prefix, pool name, etc.
  std::string compound;
  compound.reserve(key.size() + 32);
  compound.append("uc:");                   // region prefix (production: "uc:")
  compound.append(config_.pool_name);       // pool name
  compound.push_back(':');
  compound.append(key);                     // actual key
  compound.append(":v1");                   // version suffix
  return compound;
}

// Production-like GET overhead: key construction, hashing, ACL, stats, timestamps
void UcacheBenchServer::runProductionGetOverhead(
    const std::string& key, bool hit) {
  // 1. Key construction (matches createMcStoredKey)
  auto compoundKey = buildCompoundKey(key);

  // 2. Key hashing (matches getHashForKey using MurmurHash2)
  uint64_t keyHash = facebook::cachelib::MurmurHash2()(
      compoundKey.data(), compoundKey.size());
  folly::doNotOptimizeAway(keyHash);

  // 3. Timestamp reads (matches ucache::gettime() called at request start)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // 4. ACL prefix check (matches prefixAclsHandler_->checkAcl)
  // Production: extracts key prefix, looks up ACL category, checks identity
  uint64_t prefixHash = folly::hash::fnv64_buf(key.data(),
      std::min(key.size(), size_t(8)));
  auto aclIt = aclPrefixTable_.find(prefixHash & 0xFF);
  prodStats_.aclChecks.fetch_add(1, std::memory_order_relaxed);
  if (aclIt != aclPrefixTable_.end()) {
    prodStats_.aclAllowed.fetch_add(1, std::memory_order_relaxed);
  }
  folly::doNotOptimizeAway(aclIt);

  // 5. Overload protection check (matches OverloadProtector::onRequest)
  // Production reads atomic counters to check CPU and network overload
  auto inflight = prodStats_.inflightRequests.fetch_add(
      1, std::memory_order_relaxed);
  folly::doNotOptimizeAway(inflight);

  // 6. Stats tracking (matches many USTAT_INCR / STAT_INCREMENT calls)
  // Production increments: requests, keyBytesTotal, prefixCounters,
  // get_hit/get_miss, tickets_checked, inflight_requests
  prodStats_.totalRequests.fetch_add(1, std::memory_order_relaxed);
  prodStats_.keyBytesTotal.fetch_add(key.size(), std::memory_order_relaxed);
  if (hit) {
    prodStats_.getHits.fetch_add(1, std::memory_order_relaxed);
    prodStats_.prefixCounterHits.fetch_add(1, std::memory_order_relaxed);
  } else {
    prodStats_.getMisses.fetch_add(1, std::memory_order_relaxed);
  }

  // 7. Ticket staleness check (matches checkTicketStaleness)
  // Production: reads FOLLY_SETTING, compares HLC tickets
  prodStats_.ticketChecks.fetch_add(1, std::memory_order_relaxed);

  // 8. Overload check completion + egress hash
  // Production: computeAndStoreEgressHash, enforceEgressLimit
  uint64_t egressHash = folly::hash::twang_mix64(keyHash);
  folly::doNotOptimizeAway(egressHash);
  prodStats_.overloadChecks.fetch_add(1, std::memory_order_relaxed);

  // 9. Response timestamp (matches ServiceRouterLoggingHandler post-write)
  clock_gettime(CLOCK_MONOTONIC, &ts);
  folly::doNotOptimizeAway(ts);

  // Decrement inflight
  prodStats_.inflightRequests.fetch_sub(1, std::memory_order_relaxed);
}

// Production-like SET overhead
void UcacheBenchServer::runProductionSetOverhead(const std::string& key) {
  auto compoundKey = buildCompoundKey(key);
  uint64_t keyHash = facebook::cachelib::MurmurHash2()(
      compoundKey.data(), compoundKey.size());
  folly::doNotOptimizeAway(keyHash);

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  // ACL check
  uint64_t prefixHash = folly::hash::fnv64_buf(key.data(),
      std::min(key.size(), size_t(8)));
  auto aclIt = aclPrefixTable_.find(prefixHash & 0xFF);
  prodStats_.aclChecks.fetch_add(1, std::memory_order_relaxed);
  if (aclIt != aclPrefixTable_.end()) {
    prodStats_.aclAllowed.fetch_add(1, std::memory_order_relaxed);
  }

  // Stats
  auto inflight = prodStats_.inflightRequests.fetch_add(
      1, std::memory_order_relaxed);
  folly::doNotOptimizeAway(inflight);
  prodStats_.totalRequests.fetch_add(1, std::memory_order_relaxed);
  prodStats_.keyBytesTotal.fetch_add(key.size(), std::memory_order_relaxed);
  prodStats_.setStored.fetch_add(1, std::memory_order_relaxed);
  prodStats_.overloadChecks.fetch_add(1, std::memory_order_relaxed);

  clock_gettime(CLOCK_MONOTONIC, &ts);
  folly::doNotOptimizeAway(ts);

  prodStats_.inflightRequests.fetch_sub(1, std::memory_order_relaxed);
}

folly::SemiFuture<UcbGetReply> UcacheBenchServer::processUcbGet(
    const UcbGetRequest& req) {
  UcbGetReply reply;
  reply.result() = carbon::Result::NOTFOUND;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key()->fullKey().str();

    // Legacy CPU overhead simulation
    simulatePerRequestOverhead(keyStr);

    // Acquire shared bucket lock if enabled
    std::optional<BucketLocks::ReadLockHolder> bucketLock;
    if (bucketLocks_) {
      bucketLock.emplace(
          bucketLocks_->lockShared(keyStr.data(), keyStr.size()));
    }

    auto item = cache_->find(keyStr);
    bool hit = item != nullptr;

    // Production-like per-request overhead (after cache lookup, like production)
    if (config_.production_features_enabled) {
      runProductionGetOverhead(keyStr, hit);
    }

    if (hit) {
      // Cache hit
      reply.result() = carbon::Result::FOUND;

      // Set the value as IOBuf
      auto valueView = item->getMemory();
      reply.value() = *folly::IOBuf::copyBuffer(
          reinterpret_cast<const char*>(valueView), item->getSize());

      reply.flags() = req.flags().has_value() ? req.flags().value() : 0;

      recordGet(true /* hit */);

      if (config_.verbose) {
        printf("Cache hit for key: %s\n", keyStr.c_str());
      }
    } else {
      // Cache miss
      reply.result() = carbon::Result::NOTFOUND;
      recordGet(false /* miss */);

      if (config_.verbose) {
        printf("Cache miss for key: %s\n", keyStr.c_str());
      }
    }
  } catch (const std::exception& ex) {
    printf("Error processing get request: %s\n", ex.what());
    reply.result() = carbon::Result::REMOTE_ERROR;
    reply.message() = ex.what();
  }

  return folly::makeSemiFuture(std::move(reply));
}

folly::SemiFuture<UcbSetReply> UcacheBenchServer::processUcbSet(
    const UcbSetRequest& req) {
  UcbSetReply reply;
  reply.result() = carbon::Result::NOTSTORED;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key()->fullKey().str();

    // Legacy CPU overhead simulation
    simulatePerRequestOverhead(keyStr);

    // Production-like per-request overhead
    if (config_.production_features_enabled) {
      runProductionSetOverhead(keyStr);
    }

    // Acquire exclusive bucket lock if enabled
    std::optional<BucketLocks::WriteLockHolder> bucketLock;
    if (bucketLocks_) {
      bucketLock.emplace(
          bucketLocks_->lockExclusive(keyStr.data(), keyStr.size()));
    }

    // Extract value from IOBuf
    const auto& valueIoBuf = req.value();
    auto valueStr = valueIoBuf->to<std::string>();

    // Create item
    auto item = cache_->allocate(poolId_, keyStr, valueStr.size());
    if (item) {
      // Copy data to the item
      std::memcpy(item->getMemory(), valueStr.data(), valueStr.size());

      // Insert into cache - insertOrReplace always succeeds with a valid handle
      // It returns the old item handle (if replaced) or null (if new insertion)
      cache_->insertOrReplace(item);

      reply.result() = carbon::Result::STORED;
      reply.flags() = req.flags().has_value() ? req.flags().value() : 0;

      recordSet();

      if (config_.verbose) {
        printf("Stored key: %s, size: %zu\n", keyStr.c_str(), valueStr.size());
      }
    } else {
      if (config_.verbose) {
        printf(
            "ERROR: allocate failed for key: %s, size: %zu, poolId: %u\n",
            keyStr.c_str(),
            valueStr.size(),
            static_cast<uint32_t>(poolId_));
      }
      reply.result() = carbon::Result::NOTSTORED;
      reply.message() = "allocate failed";
    }
  } catch (const std::exception& ex) {
    printf("Error processing set request: %s\n", ex.what());
    reply.result() = carbon::Result::REMOTE_ERROR;
    reply.message() = ex.what();
  }

  return folly::makeSemiFuture(std::move(reply));
}

folly::SemiFuture<UcbDeleteReply> UcacheBenchServer::processUcbDelete(
    const UcbDeleteRequest& req) {
  UcbDeleteReply reply;
  reply.result() = carbon::Result::NOTFOUND;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key()->fullKey().str();

    // Try to remove from cache
    auto removeResult = cache_->remove(keyStr);
    if (removeResult == CacheAllocator::RemoveRes::kSuccess) {
      reply.result() = carbon::Result::DELETED;
      reply.flags() = req.flags().has_value() ? req.flags().value() : 0;

      recordDelete();

      if (config_.verbose) {
        printf("Deleted key: %s\n", keyStr.c_str());
      }
    } else {
      reply.result() = carbon::Result::NOTFOUND;
      if (config_.verbose) {
        printf("Key not found for deletion: %s\n", keyStr.c_str());
      }
    }
  } catch (const std::exception& ex) {
    printf("Error processing delete request: %s\n", ex.what());
    reply.result() = carbon::Result::REMOTE_ERROR;
    reply.message() = ex.what();
  }

  return folly::makeSemiFuture(std::move(reply));
}

void UcacheBenchServer::printStats() {
  if (cache_) {
    // Simplified stats printing - the exact field names vary by CacheLib
    // version
    printf(
        "Cache stats available - use cache_->getGlobalCacheStats() for detailed metrics\n");
  }
}

void UcacheBenchServer::setTrackingPhase(TrackingPhase phase) {
  auto oldPhase = currentPhase_.exchange(phase);

  // Reset metrics when transitioning to a new tracking phase
  if (phase == TrackingPhase::WARMUP && oldPhase != TrackingPhase::WARMUP) {
    warmupMetrics_.reset();
    printf("[Server] Starting warmup phase metrics tracking\n");
  } else if (
      phase == TrackingPhase::BENCHMARK &&
      oldPhase != TrackingPhase::BENCHMARK) {
    benchmarkMetrics_.reset();
    printf("[Server] Starting benchmark phase metrics tracking\n");
  }
}

void UcacheBenchServer::recordGet(bool hit) {
  auto phase = currentPhase_.load();
  if (phase == TrackingPhase::WARMUP) {
    warmupMetrics_.getRequests.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
      warmupMetrics_.getHits.fetch_add(1, std::memory_order_relaxed);
    } else {
      warmupMetrics_.getMisses.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (phase == TrackingPhase::BENCHMARK) {
    benchmarkMetrics_.getRequests.fetch_add(1, std::memory_order_relaxed);
    if (hit) {
      benchmarkMetrics_.getHits.fetch_add(1, std::memory_order_relaxed);
    } else {
      benchmarkMetrics_.getMisses.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void UcacheBenchServer::recordSet() {
  auto phase = currentPhase_.load();
  if (phase == TrackingPhase::WARMUP) {
    warmupMetrics_.setRequests.fetch_add(1, std::memory_order_relaxed);
  } else if (phase == TrackingPhase::BENCHMARK) {
    benchmarkMetrics_.setRequests.fetch_add(1, std::memory_order_relaxed);
  }
}

void UcacheBenchServer::recordDelete() {
  auto phase = currentPhase_.load();
  if (phase == TrackingPhase::WARMUP) {
    warmupMetrics_.deleteRequests.fetch_add(1, std::memory_order_relaxed);
  } else if (phase == TrackingPhase::BENCHMARK) {
    benchmarkMetrics_.deleteRequests.fetch_add(1, std::memory_order_relaxed);
  }
}

void UcacheBenchServer::printFinalResults(double benchmarkDurationSec) const {
  const auto& metrics = benchmarkMetrics_;

  uint64_t getReqs = metrics.getRequests.load();
  uint64_t getHits = metrics.getHits.load();
  uint64_t getMisses = metrics.getMisses.load();
  uint64_t setReqs = metrics.setRequests.load();
  uint64_t deleteReqs = metrics.deleteRequests.load();
  uint64_t totalOps = getReqs + setReqs + deleteReqs;

  double qps =
      (benchmarkDurationSec > 0) ? totalOps / benchmarkDurationSec : 0.0;
  double hitRatio = (getReqs > 0) ? (100.0 * getHits / getReqs) : 0.0;

  // Print in format parseable by benchpress
  printf("\n");
  printf("========================================\n");
  printf("     UCACHEBENCH SERVER RESULTS\n");
  printf("========================================\n");
  printf("Benchmark Duration: %.3f seconds\n", benchmarkDurationSec);
  printf("\n");
  printf("Operations:\n");
  printf("  GET requests:    %lu\n", getReqs);
  printf("  GET hits:        %lu\n", getHits);
  printf("  GET misses:      %lu\n", getMisses);
  printf("  SET requests:    %lu\n", setReqs);
  printf("  DELETE requests: %lu\n", deleteReqs);
  printf("  Total ops:       %lu\n", totalOps);
  printf("\n");
  printf("Performance:\n");
  printf("  QPS:        %.1f\n", qps);
  printf("  Hit Ratio:  %.2f%%\n", hitRatio);
  printf("========================================\n");
  printf("\n");

  // Also print warmup stats for reference
  const auto& warmup = warmupMetrics_;
  uint64_t warmupTotalOps = warmup.getRequests.load() +
      warmup.setRequests.load() + warmup.deleteRequests.load();
  if (warmupTotalOps > 0) {
    printf("Warmup Stats (for reference):\n");
    printf(
        "  GET requests: %lu (hits: %lu, misses: %lu)\n",
        warmup.getRequests.load(),
        warmup.getHits.load(),
        warmup.getMisses.load());
    printf("  SET requests: %lu\n", warmup.setRequests.load());
    printf("  Total ops:    %lu\n", warmupTotalOps);
    printf("\n");
  }
}

void UcacheBenchServer::startPeriodicStats(uint32_t intervalSec) {
  if (intervalSec == 0) {
    return;
  }
  statsRunning_.store(true);
  statsThread_ =
      std::thread([this, intervalSec]() { periodicStatsLoop(intervalSec); });
}

void UcacheBenchServer::stopPeriodicStats() {
  if (statsRunning_.load()) {
    statsRunning_.store(false);
    statsCv_.notify_all();
    if (statsThread_.joinable()) {
      statsThread_.join();
    }
  }
}

void UcacheBenchServer::periodicStatsLoop(uint32_t intervalSec) {
  // Previous snapshot for computing interval QPS
  uint64_t prevTotalOps = 0;
  auto prevTime = std::chrono::steady_clock::now();
  auto phaseStartTime = prevTime;
  TrackingPhase prevPhase = TrackingPhase::NONE;

  while (statsRunning_.load()) {
    {
      std::unique_lock<std::mutex> lock(statsMutex_);
      statsCv_.wait_for(lock, std::chrono::seconds(intervalSec), [this]() {
        return !statsRunning_.load();
      });
    }

    if (!statsRunning_.load()) {
      break;
    }

    auto phase = currentPhase_.load();
    if (phase == TrackingPhase::NONE) {
      continue;
    }

    // Reset snapshot on phase transition and skip this iteration.
    // Without the continue, intervalElapsed would be nearly zero (since
    // prevTime was just set) while totalOps could already have accumulated
    // operations, producing an astronomically high intervalQps spike.
    if (phase != prevPhase) {
      prevTotalOps = 0;
      prevTime = std::chrono::steady_clock::now();
      phaseStartTime = prevTime;
      prevPhase = phase;
      continue;
    }

    const PhaseMetrics& metrics =
        (phase == TrackingPhase::WARMUP) ? warmupMetrics_ : benchmarkMetrics_;

    uint64_t getReqs = metrics.getRequests.load(std::memory_order_relaxed);
    uint64_t getHits = metrics.getHits.load(std::memory_order_relaxed);
    uint64_t setReqs = metrics.setRequests.load(std::memory_order_relaxed);
    uint64_t deleteReqs =
        metrics.deleteRequests.load(std::memory_order_relaxed);
    uint64_t totalOps = getReqs + setReqs + deleteReqs;

    auto now = std::chrono::steady_clock::now();
    double intervalElapsed =
        std::chrono::duration<double>(now - prevTime).count();
    double phaseElapsed =
        std::chrono::duration<double>(now - phaseStartTime).count();

    double intervalQps = (intervalElapsed > 0)
        ? (totalOps - prevTotalOps) / intervalElapsed
        : 0.0;
    double avgQps = (phaseElapsed > 0) ? totalOps / phaseElapsed : 0.0;
    double hitRatio = (getReqs > 0) ? (100.0 * getHits / getReqs) : 0.0;

    const char* phaseName =
        (phase == TrackingPhase::WARMUP) ? "WARMUP" : "BENCHMARK";

    printf(
        "[Server %s] %.0fs elapsed | QPS: %.0f (avg: %.0f) | "
        "hit_ratio: %.2f%% | ops: %lu (GET: %lu, SET: %lu, DEL: %lu)\n",
        phaseName,
        phaseElapsed,
        intervalQps,
        avgQps,
        hitRatio,
        totalOps,
        getReqs,
        setReqs,
        deleteReqs);
    fflush(stdout);

    prevTotalOps = totalOps;
    prevTime = now;
  }
}

} // namespace ucachebench
} // namespace facebook
