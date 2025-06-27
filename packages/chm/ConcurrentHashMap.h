/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

#pragma once

/**
 * ConcurrentHashMap.h
 *
 * A high-performance concurrent hash map implementation using sharding
 * technique for multi-threaded environments. This implementation divides the
 * hash space into multiple independent shards, each protected by its own mutex,
 * to reduce contention.
 */

#include <folly/Likely.h>
#include <folly/SharedMutex.h>
#include <folly/container/F14Map.h>
#include <folly/lang/Aligned.h>
#include <folly/synchronization/Lock.h>
#include <array>
#include <atomic>
#include <memory>
#include <utility>

/**
 * Hash function for shard distribution
 *
 * @param key The input key to hash
 * @return A well-distributed hash value for sharding
 */
inline int32_t shardingHashFromKey(int32_t key) {
  constexpr const uint32_t kC = 0x27d4eb2d; // a prime and an odd constant
  key ^= ((key >> 15) | (static_cast<uint32_t>(key) << 17));
  return static_cast<int32_t>(key * kC);
}

/**
 * ConcurrentHashMap - Thread-safe hash map implementation using sharding
 *
 * This implementation divides the hash space into multiple shards, each with
 * its own mutex and underlying hash map, to reduce lock contention in
 * concurrent access.
 */
template <
    typename KeyT, // Key type
    typename RecordT, // Value type
    size_t kShardBits = 8, // Number of shards as power of 2
    typename InternalHashMapType =
        folly::F14FastMap<KeyT, RecordT>, // Underlying map implementation
    typename MutexType = folly::SharedMutex> // Mutex type for synchronization
class ConcurrentHashMap {
  enum {
    kNumShards = (1 << kShardBits), // Calculate number of shards (2^kShardBits)
    kShardMask = kNumShards - 1, // Bitmask for fast modulo operation
  };

  using HashFunc = typename InternalHashMapType::hasher;

 public:
  // Standard container type definitions
  using key_type = KeyT;
  using data_type = RecordT;
  using mapped_type = RecordT;
  using value_type = std::pair<KeyT, RecordT>;

  /**
   * Constructor with optional size hint
   *
   * @param sizeHint Expected number of elements (distributed across shards)
   */
  explicit ConcurrentHashMap(int sizeHint = 0) {
    size_t hint = sizeHint >> kShardBits; // Divide hint by number of shards
    for (int i = 0; i < kNumShards; ++i) {
      maps_[i].reset(new InternalHashMapType(hint));
    }
  }

  ~ConcurrentHashMap() = default;

  /**
   * Retrieves a value by key
   *
   * @param key The key to look up
   * @return Pair of (value, found_flag) where found_flag is true if key exists
   */
  std::pair<data_type, bool> getValue(const key_type& key) const {
    size_t shard =
        keyToSubMapIndex(key); // Determine which shard contains this key
    {
      std::shared_lock lock{*mutexes_[shard]}; // Acquire lock for this shard
      auto iter = subMap(shard).find(key);
      if (iter != subMap(shard).end()) {
        return std::pair<data_type, bool>(iter->second, true);
      }
    }
    return std::pair<data_type, bool>(
        data_type(), false); // Return default value if not found
  }

  /**
   * Inserts a key-value pair into the map
   *
   * @param in The key-value pair to insert
   * @return Pair of (stored_value, inserted_flag) where inserted_flag is true
   * if newly inserted
   */
  std::pair<data_type, bool> put(const value_type& in) {
    return put(in.first, in.second);
  }

  /**
   * Inserts a key-value pair into the map
   *
   * @param key The key to insert
   * @param value The value to associate with the key
   * @return Pair of (stored_value, inserted_flag) where inserted_flag is true
   * if newly inserted
   */
  std::pair<data_type, bool> put(const key_type& key, const data_type& value) {
    size_t shard =
        keyToSubMapIndex(key); // Determine which shard this key belongs to
    std::unique_lock xlock{
        *mutexes_[shard]}; // Acquire exclusive lock for this shard
    auto ret(subMap(shard).insert(value_type(key, value)));
    return std::pair<data_type, bool>(ret.first->second, ret.second);
  }

 private:
  /**
   * Maps a key to its corresponding shard index
   *
   * @param key The key to map
   * @return The shard index (0 to kNumShards-1)
   */
  static int32_t keyToSubMapIndex(KeyT key) {
    // Use shardingHashFromKey for better distribution
    return shardingHashFromKey(HashFunc()(key)) & kShardMask;
  }

  /**
   * Access the underlying map for a specific shard (const version)
   */
  const InternalHashMapType& subMap(size_t shard) const {
    return *maps_[shard];
  }

  /**
   * Access the underlying map for a specific shard (non-const version)
   */
  InternalHashMapType& subMap(size_t shard) {
    return *maps_[shard];
  }

  // Array of mutexes, one per shard, cache-line aligned to prevent false
  // sharing
  mutable std::array<folly::cacheline_aligned<MutexType>, kNumShards> mutexes_;

  // Array of hash maps, one per shard
  std::unique_ptr<InternalHashMapType> maps_[kNumShards];
};
