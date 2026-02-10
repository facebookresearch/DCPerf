/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchServer.h"

#include <folly/Format.h>
#include <folly/portability/GFlags.h>

#include "cachelib/allocator/CacheAllocator.h"
#include "cachelib/allocator/HitsPerSlabStrategy.h"

using namespace facebook::ucachebench;

namespace facebook {
namespace ucachebench {

UcacheBenchServer::UcacheBenchServer(const UcacheBenchConfig& config)
    : config_(config) {
  setupCacheLib();
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
    // Enable pool rebalancing with HitsPerSlab strategy
    facebook::cachelib::HitsPerSlabStrategy::Config hitsConfig;
    hitsConfig.minLruTailAge = config_.lru_rebalancing_hits_min_age_sec;
    hitsConfig.maxLruTailAge = config_.lru_rebalancing_hits_max_age_sec;

    auto rebalanceStrategy =
        std::make_shared<facebook::cachelib::HitsPerSlabStrategy>(hitsConfig);
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

folly::SemiFuture<UcbGetReply> UcacheBenchServer::processUcbGet(
    const UcbGetRequest& req) {
  UcbGetReply reply;
  reply.result_ref() = carbon::Result::NOTFOUND;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key_ref()->fullKey().str();

    auto item = cache_->find(keyStr);
    if (item) {
      // Cache hit
      reply.result_ref() = carbon::Result::FOUND;

      // Set the value as IOBuf
      auto valueView = item->getMemory();
      reply.value_ref() = *folly::IOBuf::copyBuffer(
          reinterpret_cast<const char*>(valueView), item->getSize());

      reply.flags_ref() =
          req.flags_ref().has_value() ? req.flags_ref().value() : 0;

      if (config_.verbose) {
        printf("Cache hit for key: %s\n", keyStr.c_str());
      }
    } else {
      // Cache miss
      reply.result_ref() = carbon::Result::NOTFOUND;
      if (config_.verbose) {
        printf("Cache miss for key: %s\n", keyStr.c_str());
      }
    }
  } catch (const std::exception& ex) {
    printf("Error processing get request: %s\n", ex.what());
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
  }

  return folly::makeSemiFuture(std::move(reply));
}

folly::SemiFuture<UcbSetReply> UcacheBenchServer::processUcbSet(
    const UcbSetRequest& req) {
  UcbSetReply reply;
  reply.result_ref() = carbon::Result::NOTSTORED;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key_ref()->fullKey().str();

    // Extract value from IOBuf (need to work with the const IOBuf)
    const auto& valueIoBuf = req.value_ref();
    auto valueStr = valueIoBuf->to<std::string>();

    // Create item
    auto item = cache_->allocate(poolId_, keyStr, valueStr.size());
    if (item) {
      // Copy data to the item
      std::memcpy(item->getMemory(), valueStr.data(), valueStr.size());

      // Insert into cache - insertOrReplace always succeeds with a valid handle
      // It returns the old item handle (if replaced) or null (if new insertion)
      cache_->insertOrReplace(item);

      reply.result_ref() = carbon::Result::STORED;
      reply.flags_ref() =
          req.flags_ref().has_value() ? req.flags_ref().value() : 0;

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
      reply.result_ref() = carbon::Result::NOTSTORED;
      reply.message_ref() = "allocate failed";
    }
  } catch (const std::exception& ex) {
    printf("Error processing set request: %s\n", ex.what());
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
  }

  return folly::makeSemiFuture(std::move(reply));
}

folly::SemiFuture<UcbDeleteReply> UcacheBenchServer::processUcbDelete(
    const UcbDeleteRequest& req) {
  UcbDeleteReply reply;
  reply.result_ref() = carbon::Result::NOTFOUND;

  try {
    // Extract key from Carbon Keys type - convert to string
    std::string keyStr = req.key_ref()->fullKey().str();

    // Try to remove from cache
    auto removeResult = cache_->remove(keyStr);
    if (removeResult == CacheAllocator::RemoveRes::kSuccess) {
      reply.result_ref() = carbon::Result::DELETED;
      reply.flags_ref() =
          req.flags_ref().has_value() ? req.flags_ref().value() : 0;

      if (config_.verbose) {
        printf("Deleted key: %s\n", keyStr.c_str());
      }
    } else {
      reply.result_ref() = carbon::Result::NOTFOUND;
      if (config_.verbose) {
        printf("Key not found for deletion: %s\n", keyStr.c_str());
      }
    }
  } catch (const std::exception& ex) {
    printf("Error processing delete request: %s\n", ex.what());
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
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

} // namespace ucachebench
} // namespace facebook
