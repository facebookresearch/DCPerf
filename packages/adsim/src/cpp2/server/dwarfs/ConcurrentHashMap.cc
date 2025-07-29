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

#include <cea/chips/adsim/cpp2/server/dwarfs/ConcurrentHashMap.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(concurrent_hashmap_nfired);

/* A closure to create a vector of keys of certain type
 *
 * @return  The reference to the vector of keys
 */
template <class K>
std::vector<K>& ConcurrentHashMap::keyList() {
  static std::vector<K> keys;
  return keys;
}

/* A closure to provide the lock for keylist of certain type
 *
 * @return  The reference to the lock
 */
folly::Synchronized<int>& ConcurrentHashMap::keyListLock() {
  static folly::Synchronized<int> key_list_lock;
  return key_list_lock;
}

/* ConcurrentHashMap kernel constructor
 *
 * Initializes benchmark with frequency-based access patterns from distribution
 * file. Creates large pregenerated request book for realistic workload
 * simulation.
 */
ConcurrentHashMap::ConcurrentHashMap(
    int requests_per_run,
    int request_book_multiplier,
    int num_threads,
    std::string input_str,
    std::string distribution_file)
    : requests_per_run_(requests_per_run),
      request_book_multiplier_(request_book_multiplier),
      num_threads_(num_threads),
      input_str_(std::move(input_str)),
      distribution_file_(std::move(distribution_file)),
      current_request_position_(0) {
  LOG(INFO) << "ConcurrentHashMap initialized with params: "
            << "requests_per_run=" << requests_per_run_
            << ", request_book_multiplier=" << request_book_multiplier_
            << ", num_threads=" << num_threads_
            << ", distribution_file=" << distribution_file_;
}

/* Create and populate concurrent hash map in handler objects
 *
 * Instantiates sharded hash map and populates with key-value pairs.
 * Values are AdData objects created with sequential AdId values.
 */
void ConcurrentHashMap::insert_map(AdSimHandleObjs* h_objs, size_t total_keys) {
  auto m = std::make_shared<concurrent_map<AdId, AdDataPtr>>(total_keys);
  for (size_t i = 0; i < total_keys; ++i) {
    AdId adId = static_cast<AdId>(i);
    m->put(adId, std::make_shared<AdData>(adId));
  }
  h_objs->set_shared_ptr(input_str_, m);
}

/* Generate large pregenerated request book based on frequency distribution
 *
 * Creates request book with (request_book_multiplier × dataset_size) entries.
 * Uses frequency-weighted random sampling from distribution buckets.
 */
void ConcurrentHashMap::generatePregeneratedRequests(
    const WorkloadGenerator& generator) {
  size_t dataset_size = keyList<AdId>().size();
  size_t request_book_size = request_book_multiplier_ * dataset_size;
  pregenerated_requests_.clear();
  pregenerated_requests_.reserve(request_book_size);

  // Create frequency buckets and weighted access distribution
  auto buckets = generator.createBuckets();
  auto accessDistribution = generator.createAccessDistribution();

  std::random_device rd;
  std::mt19937 rng(rd());

  // Generate request book with frequency-weighted random selection
  for (size_t i = 0; i < request_book_size; ++i) {
    // Select bucket based on frequency weights
    if (accessDistribution.empty() || buckets.empty()) {
      throw std::runtime_error("Empty access distribution or bucket vector");
    }
    size_t bucketIdx = accessDistribution[rng() % accessDistribution.size()];
    const auto& bucket = buckets[bucketIdx];

    // Select random key within the bucket
    size_t keyOffset = rng() % bucket.numKeys;
    size_t keyIndex = bucket.startIndex + keyOffset;
    keyIndex =
        std::min(keyIndex, static_cast<size_t>(keyList<AdId>().size() - 1));

    pregenerated_requests_.push_back(static_cast<uint32_t>(keyIndex));
  }

  LOG(INFO) << "Generated request book: " << request_book_size
            << " requests from " << dataset_size << " keys";
}

/* Initialize concurrent hash map with distribution-based workload
 *
 * Loads distribution from CSV, calculates dataset size, generates shuffled
 * working set, creates request book, and populates hash map.
 */
std::string ConcurrentHashMap::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> /*unused*/) {
  size_t total_keys = 0;

  keyListLock().withWLock([&](int& /*unused*/) {
    // Parse distribution file (frequency,count CSV format)
    DistributionReader reader(distribution_file_);
    auto distributionOpt = reader.read();
    if (distributionOpt && !distributionOpt->empty()) {
      WorkloadGenerator generator(*distributionOpt);

      // Calculate total keys by summing all count values from distribution
      total_keys = generator.calculateWorkingSetSize();
      // Populate key list with shuffled working set to avoid cache patterns
      auto& keys = keyList<AdId>();
      keys.clear();
      keys.reserve(total_keys);
      for (size_t i = 0; i < total_keys; ++i) {
        keys.push_back(static_cast<AdId>(i));
      }

      // Generate large request book for benchmark execution
      generatePregeneratedRequests(generator);

      LOG(INFO) << "Distribution loaded: " << distributionOpt->size()
                << " frequency buckets, " << total_keys << " total keys";
    } else {
      throw std::runtime_error(
          "Failed to read distribution file: " + distribution_file_);
    }
  });

  // Create and populate the concurrent hash map
  insert_map(h_objs.get(), total_keys);

  std::string info = folly::to<std::string>(
      "ConcurrentHashMap: ", input_str_, " ", std::to_string(total_keys), "B");
  info += " (distribution: " + distribution_file_ + ")";
  info += " (pregenerated " + std::to_string(pregenerated_requests_.size()) +
      " requests)";
  return info;
}

/* Single-threaded lookup using pregenerated request sequence
 *
 * Executes requests_per_run_ lookups from request book with atomic position
 * advancement and wrapping. XOR aggregation prevents compiler optimization.
 */
int ConcurrentHashMap::lookup_map(std::shared_ptr<AdSimHandleObjs> h_objs) {
  auto m = h_objs->get_shared_ptr<concurrent_map<AdId, AdDataPtr>>(input_str_);
  int x = 0;

  // Atomically reserve request range and advance global position
  uint32_t start_pos = current_request_position_.fetch_add(
                           static_cast<uint32_t>(requests_per_run_)) %
      pregenerated_requests_.size();

  // Execute lookups with automatic wrapping around request book
  for (uint32_t i = 0; i < requests_per_run_; ++i) {
    uint32_t pos = (start_pos + i) % pregenerated_requests_.size();
    uint32_t requestIdx = pregenerated_requests_[pos];
    AdId adId = static_cast<AdId>(requestIdx);
    auto result = m->getValue(adId);
    if (result.second) {
      x ^= static_cast<int>(
          result.first->getId()); // XOR aggregation for validation
    }
  }

  return x;
}

/* Multi-threaded concurrent lookup with work distribution
 *
 * Distributes requests_per_run_ lookups across worker threads. Each thread
 * processes contiguous chunk from reserved request range. XOR aggregation.
 */
folly::coro::Task<int> ConcurrentHashMap::concurrent_lookup_map(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<folly::Executor> pool) {
  auto m = h_objs->get_shared_ptr<concurrent_map<AdId, AdDataPtr>>(input_str_);

  // Atomically reserve contiguous request range for this fire operation
  uint32_t start_pos = current_request_position_.fetch_add(
                           static_cast<uint32_t>(requests_per_run_)) %
      pregenerated_requests_.size();

  std::vector<folly::Future<int>> futures;
  uint32_t requests_per_thread = static_cast<uint32_t>(requests_per_run_) /
      static_cast<uint32_t>(num_threads_);

  // Create worker tasks with balanced load distribution
  for (int t = 0; t < num_threads_; ++t) {
    uint32_t thread_start = t * requests_per_thread;
    uint32_t thread_end = (t == num_threads_ - 1)
        ? static_cast<uint32_t>(
              requests_per_run_) // Last thread handles remainder
        : (t + 1) * requests_per_thread;

    auto future = folly::via(
        pool.get(), [this, m, start_pos, thread_start, thread_end]() {
          int x = 0;
          // Process assigned chunk of requests with wrapping
          for (uint32_t i = thread_start; i < thread_end; ++i) {
            uint32_t pos = (start_pos + i) % pregenerated_requests_.size();
            uint32_t requestIdx = pregenerated_requests_[pos];
            AdId adId = static_cast<AdId>(requestIdx);
            auto result = m->getValue(adId);
            if (result.second) {
              x ^= static_cast<int>(
                  result.first->getId()); // XOR aggregation per thread
            }
          }
          return x;
        });
    futures.push_back(std::move(future));
  }

  // Collect and aggregate results from all worker threads
  auto results = co_await folly::collectAll(futures);
  int final_result = 0;
  for (auto& result : results) {
    if (result.hasValue()) {
      final_result ^= result.value();
    }
  }

  co_return final_result;
}

/* Execute benchmark fire operation
 *
 * Performs single benchmark iteration with requests_per_run_ lookups.
 * Selects single/multi-threaded execution based on configuration.
 */
folly::coro::Task<std::string> ConcurrentHashMap::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool) {
  STATS_concurrent_hashmap_nfired.add(1);

  // Select execution mode based on thread configuration and pool availability
  if (num_threads_ > 1 && pool) {
    co_await concurrent_lookup_map(h_objs, pool);
  } else {
    lookup_map(h_objs);
  }

  co_return "";
}

} // namespace facebook::cea::chips::adsim
