/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/Likely.h>
#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/lang/Aligned.h>
#include <folly/synchronization/Lock.h>

namespace facebook::cea::chips::adsim {

using AdId = int64_t;

// Mock advertisement data stored as values in the hash map
class AdData {
 public:
  explicit AdData(AdId id) : id_(id), data_("AdData-" + std::to_string(id)) {}

  AdId getId() const {
    return id_;
  }
  const std::string& getData() const {
    return data_;
  }

 private:
  AdId id_;
  std::string data_; // Simulated payload data
};

using AdDataPtr = std::shared_ptr<AdData>;

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
 * Represents frequency distribution data from CSV input
 */
struct DistributionEntry {
  uint64_t frequency; // Access frequency weight
  uint64_t numKeys; // Number of keys in this frequency bucket
};

/**
 * Parses distribution file in CSV format (frequency,numKeys)
 */
class DistributionReader {
 public:
  explicit DistributionReader(std::string_view filePath)
      : filePath_(filePath) {}

  std::optional<std::vector<DistributionEntry>> read() const {
    std::vector<DistributionEntry> distribution;
    std::ifstream file(filePath_);

    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string line;
    bool isFirstLine = true;
    while (std::getline(file, line)) {
      if (isFirstLine) {
        isFirstLine = false;
        continue;
      }
      if (auto entry = parseLine(line)) {
        distribution.push_back(*entry);
      }
    }

    return distribution;
  }

 private:
  std::optional<DistributionEntry> parseLine(const std::string& line) const {
    std::istringstream iss(line);
    std::string freqStr, numKeysStr;

    if (std::getline(iss, freqStr, ',') && std::getline(iss, numKeysStr)) {
      try {
        return DistributionEntry{
            .frequency = static_cast<uint64_t>(std::stod(freqStr)),
            .numKeys = static_cast<uint64_t>(std::stoll(numKeysStr))};
      } catch (const std::exception&) {
        // Skip invalid lines
      }
    }
    return std::nullopt;
  }

  std::string filePath_;
};

/**
 * Generates workloads based on frequency distribution patterns
 */
class WorkloadGenerator {
 public:
  struct Bucket {
    size_t startIndex; // Start index in shuffled working set
    uint64_t numKeys; // Number of keys in this bucket
  };

  explicit WorkloadGenerator(const std::vector<DistributionEntry>& distribution)
      : distribution_(distribution) {}

  uint64_t calculateWorkingSetSize() const {
    uint64_t totalKeys = 0;
    for (const auto& entry : distribution_) {
      totalKeys += entry.numKeys;
    }
    return totalKeys;
  }

  std::vector<Bucket> createBuckets() const {
    std::vector<Bucket> buckets;
    buckets.reserve(distribution_.size());
    size_t currentIndex = 0;

    for (const auto& entry : distribution_) {
      buckets.push_back({currentIndex, entry.numKeys});
      currentIndex += entry.numKeys;
    }

    return buckets;
  }

  std::vector<size_t> createAccessDistribution() const {
    constexpr uint64_t MAX_DISTRIBUTION_ENTRIES = 10000000;

    uint64_t totalFrequency = 0;
    for (const auto& entry : distribution_) {
      totalFrequency += entry.frequency * entry.numKeys;
    }

    uint64_t totalEntries = std::min(totalFrequency, MAX_DISTRIBUTION_ENTRIES);
    double scalingFactor = static_cast<double>(totalEntries) / totalFrequency;

    std::vector<size_t> accessDistribution;
    accessDistribution.reserve(totalEntries);

    for (size_t i = 0; i < distribution_.size(); ++i) {
      const auto& entry = distribution_[i];
      uint64_t numAppearances = std::max(
          static_cast<uint64_t>(
              entry.frequency * entry.numKeys * scalingFactor),
          uint64_t(1));

      for (uint64_t j = 0; j < numAppearances; ++j) {
        accessDistribution.push_back(i);
      }
    }

    std::random_shuffle(accessDistribution.begin(), accessDistribution.end());

    return accessDistribution;
  }

 private:
  const std::vector<DistributionEntry>& distribution_;
};

/**
 * ConcurrentHashMapImpl - Thread-safe hash map implementation using sharding
 *
 * This implementation divides the hash space into multiple shards, each with
 * its own mutex and underlying hash map, to reduce lock contention in
 * concurrent access.
 */
template <
    typename KeyT,
    typename RecordT,
    size_t kShardBits = 8,
    typename InternalHashMapType = folly::F14FastMap<KeyT, RecordT>,
    typename MutexType = folly::SharedMutex>
class ConcurrentHashMapImpl {
  enum {
    kNumShards = (1 << kShardBits),
    kShardMask = kNumShards - 1,
  };

  using HashFunc = typename InternalHashMapType::hasher;

 public:
  using key_type = KeyT;
  using data_type = RecordT;
  using mapped_type = RecordT;
  using value_type = std::pair<KeyT, RecordT>;

  explicit ConcurrentHashMapImpl(int sizeHint = 0) {
    size_t hint = sizeHint >> kShardBits;
    for (int i = 0; i < kNumShards; ++i) {
      maps_[i].reset(new InternalHashMapType(hint));
    }
  }

  ~ConcurrentHashMapImpl() = default;

  std::pair<data_type, bool> getValue(const key_type& key) const {
    std::pair<data_type, bool> ret;
    int32_t shard = keyToSubMapIndex(key);
    {
      std::shared_lock lock{*mutexes_[shard]};
      auto iter = subMap(shard).find(key);
      if (iter != subMap(shard).end()) {
        ret.first = iter->second;
        ret.second = true;
      }
    }
    return ret;
  }

  std::pair<data_type, bool> put(const value_type& in) {
    return put(in.first, in.second);
  }

  std::pair<data_type, bool> put(const key_type& key, const data_type& value) {
    int32_t shard = keyToSubMapIndex(key);
    std::unique_lock xlock{*mutexes_[shard]};
    auto ret(subMap(shard).insert(value_type(key, value)));
    return std::pair<data_type, bool>(ret.first->second, ret.second);
  }

 private:
  static int32_t keyToSubMapIndex(const key_type& key) {
    return shardingHashFromKey(static_cast<int32_t>(HashFunc()(key))) &
        kShardMask;
  }

  const InternalHashMapType& subMap(size_t shard) const {
    return *maps_[shard];
  }

  InternalHashMapType& subMap(size_t shard) {
    return *maps_[shard];
  }

  mutable std::array<folly::cacheline_aligned<MutexType>, kNumShards> mutexes_;
  std::unique_ptr<InternalHashMapType> maps_[kNumShards];
};

/* A kernel that performs concurrent hash map operations intensively */
class ConcurrentHashMap : public Kernel {
  template <class K, class V>
  using concurrent_map = ConcurrentHashMapImpl<K, V>;

 public:
  explicit ConcurrentHashMap(
      int requests_per_run,
      int request_book_multiplier,
      int num_threads,
      std::string input_str,
      std::string distribution_file);

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;

  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int64_t requests_per_run =
        static_cast<int64_t>(config_d["requests_per_run"].asInt());
    int64_t request_book_multiplier = config_d.count("request_book_multiplier")
        ? static_cast<int64_t>(config_d["request_book_multiplier"].asInt())
        : 10;
    int64_t num_threads = config_d.count("num_threads")
        ? static_cast<int64_t>(config_d["num_threads"].asInt())
        : 4;
    std::string input_str = "ConcurrentHashMap";
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    std::string distribution_file = config_d["distribution_file"].asString();
    return std::make_shared<ConcurrentHashMap>(
        requests_per_run,
        request_book_multiplier,
        num_threads,
        input_str,
        distribution_file);
  }

 private:
  int64_t requests_per_run_;
  int64_t request_book_multiplier_;
  int64_t num_threads_;
  std::string input_str_;
  std::string distribution_file_;
  std::vector<uint32_t>
      pregenerated_requests_; // Large pre-generated request book
  mutable std::atomic<uint32_t>
      current_request_position_; // Current position in request book

  folly::Synchronized<int>& keyListLock();

  void insert_map(AdSimHandleObjs* h_objs, size_t total_keys);
  int lookup_map(std::shared_ptr<AdSimHandleObjs> h_objs);
  folly::coro::Task<int> concurrent_lookup_map(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<folly::Executor> pool);

  // Helper methods for pregenerated requests
  void generatePregeneratedRequests(const WorkloadGenerator& generator);

  template <class K>
  std::vector<K>& keyList();

  template <class K>
  const K& key(size_t index) {
    return keyList<K>()[index];
  }
};

} // namespace facebook::cea::chips::adsim
