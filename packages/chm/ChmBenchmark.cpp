/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

/**
 * ConcurrentHashMap Benchmark (ChmBench)
 *
 * A benchmark for measuring ConcurrentHashMap read performance
 * with configurable access patterns based on frequency distributions.
 *
 */

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <folly/Format.h>
#include <gflags/gflags.h>

#include "./ConcurrentHashMap.h"

// Define command line flags
DEFINE_string(distribution_file, "", "Path to the distribution CSV file");
DEFINE_int32(num_threads, 4, "Number of threads for benchmark");
DEFINE_int32(duration_seconds, 10, "Duration of the benchmark in seconds");
DEFINE_int32(
    initial_capacity,
    1000000,
    "Initial capacity hint for the hash map");
DEFINE_bool(verbose, false, "Print verbose output");

namespace chm_benchmark {

// Type definitions
using AdId = int64_t;
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// Structure to hold distribution data from input file
// Each entry represents a bucket with frequency and number of keys
struct DistributionEntry {
  uint64_t frequency; // How often keys in this bucket should be accessed
  uint64_t numKeys; // Number of keys in this bucket
};

// AdData: Value type stored in the ConcurrentHashMap
// Simulates a typical value object with an ID and associated data
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
  std::string data_;
};

using AdDataPtr = std::shared_ptr<AdData>;

// Type definition for the ConcurrentHashMap under test
// Uses 8 shards for concurrency, F14FastMap for internal storage,
// and SharedMutex for thread synchronization
using ConcurrentHashMapType = ConcurrentHashMap<
    AdId, // key type
    AdDataPtr, // value type
    8, // shard bits
    folly::F14FastMap<AdId, AdDataPtr>, // internal hashmap type
    folly::SharedMutex // internal mutex type
    >;

// DistributionReader: Parses the distribution file into DistributionEntry
// objects Format: frequency,numKeys (CSV format)
class DistributionReader {
 public:
  explicit DistributionReader(std::string_view filePath)
      : filePath_(filePath) {}

  std::optional<std::vector<DistributionEntry>> read() const {
    std::vector<DistributionEntry> distribution;
    std::ifstream file(filePath_);

    if (!file.is_open()) {
      std::cerr << "Error: Could not open distribution file: " << filePath_
                << std::endl;
      return std::nullopt;
    }

    std::string line;
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      std::string freqStr, numKeysStr;

      if (std::getline(iss, freqStr, ',') && std::getline(iss, numKeysStr)) {
        try {
          DistributionEntry entry;
          entry.frequency = std::stoll(freqStr);
          entry.numKeys = std::stoll(numKeysStr);
          distribution.push_back(entry);
        } catch (const std::exception& e) {
          std::cerr << "Error parsing line: " << line << " - " << e.what()
                    << std::endl;
        }
      }
    }

    return distribution;
  }

 private:
  std::string filePath_;
};

// Workload generator class
class WorkloadGenerator {
 public:
  // BucketSampler: Memory-optimized sampler for O(1) random key selection
  // Uses pre-generated distribution array and shuffled working set to:
  // 1. Maintain exact frequency distribution from input file
  // 2. Provide O(1) random key selection
  // 3. Randomize key access patterns to avoid cache effects
  class BucketSampler {
   public:
    // Bucket: Represents a range of keys with the same frequency
    // Points to a range in the shuffled working set rather than
    // using contiguous key ranges to avoid access patterns
    struct Bucket {
      size_t startIndex; // Starting index in the shuffled workingSet
      uint64_t numKeys; // Number of keys in this bucket
    };

    BucketSampler(
        const std::vector<DistributionEntry>& distribution,
        const std::vector<AdId>& workingSet) {
      // Calculate total frequency for normalization
      uint64_t totalFrequency = 0;
      for (const auto& entry : distribution) {
        totalFrequency += entry.frequency * entry.numKeys;
      }

      // Create a shuffled copy of the working set to randomize key access
      // patterns This prevents cache effects from biasing the benchmark results
      shuffledWorkingSet_ = workingSet;
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(shuffledWorkingSet_.begin(), shuffledWorkingSet_.end(), g);

      // Build buckets with indices into the shuffled working set
      buckets_.reserve(distribution.size());
      size_t currentIndex = 0;

      for (const auto& entry : distribution) {
        // Add bucket
        Bucket bucket;
        bucket.startIndex = currentIndex;
        bucket.numKeys = entry.numKeys;
        buckets_.push_back(bucket);

        // Update current index
        currentIndex += entry.numKeys;
      }

      // Pre-generate access distribution array for O(1) bucket selection
      // First, calculate the total number of entries needed
      // We'll scale down if needed to avoid excessive memory usage
      constexpr uint64_t maxEntries =
          10000000; // Cap at 10 million entries for memory efficiency
      uint64_t totalEntries = std::min(totalFrequency, maxEntries);

      // Calculate scaling factor if needed
      double scalingFactor = static_cast<double>(totalEntries) / totalFrequency;

      // Reserve space for the access distribution array
      accessDistribution_.reserve(totalEntries);

      // Fill the access distribution array
      for (size_t i = 0; i < buckets_.size(); ++i) {
        const auto& entry = distribution[i];
        // Calculate how many times this bucket should appear in the array
        uint64_t numAppearances = static_cast<uint64_t>(
            entry.frequency * entry.numKeys * scalingFactor);
        // Ensure each bucket appears at least once
        numAppearances = std::max(numAppearances, uint64_t(1));

        // Add bucket index to the access distribution array
        for (uint64_t j = 0; j < numAppearances; ++j) {
          accessDistribution_.push_back(i);
        }
      }

      // Shuffle the access distribution array to avoid patterns
      std::shuffle(accessDistribution_.begin(), accessDistribution_.end(), g);
    }

    // getRandomKey: Select a random key according to the frequency distribution
    // - Uses standard library distributions for simplicity
    // - Returns keys from shuffled working set to avoid access patterns
    AdId getRandomKey(std::mt19937& rng) const {
      if (accessDistribution_.empty() || buckets_.empty() ||
          shuffledWorkingSet_.empty()) {
        return 0;
      }

      // Select a random bucket index from the pre-generated distribution
      std::uniform_int_distribution<size_t> distIdx(
          0, accessDistribution_.size() - 1);
      size_t bucketIdx = accessDistribution_[distIdx(rng)];

      // Get the selected bucket
      const Bucket& bucket = buckets_[bucketIdx];

      // Uniformly select a key from the bucket
      std::uniform_int_distribution<uint64_t> keyDist(0, bucket.numKeys - 1);
      uint64_t keyOffset = keyDist(rng);

      // Return the key from the shuffled working set
      return shuffledWorkingSet_[bucket.startIndex + keyOffset];
    }

    // Get the number of buckets
    size_t numBuckets() const {
      return buckets_.size();
    }

    // Get the size of the access distribution array
    size_t distributionSize() const {
      return accessDistribution_.size();
    }

   private:
    std::vector<Bucket> buckets_;
    std::vector<size_t>
        accessDistribution_; // Pre-generated distribution of bucket indices
    std::vector<AdId> shuffledWorkingSet_; // Shuffled copy of the working set
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

  uint64_t calculateTotalFrequency() const {
    uint64_t totalFrequency = 0;
    for (const auto& entry : distribution_) {
      totalFrequency += entry.frequency * entry.numKeys;
    }
    return totalFrequency;
  }

  std::vector<AdId> generateWorkingSet() const {
    std::vector<AdId> workingSet;
    workingSet.reserve(calculateWorkingSetSize());
    int64_t keyId = 0;

    for (const auto& entry : distribution_) {
      for (int64_t i = 0; i < entry.numKeys; ++i) {
        workingSet.push_back(keyId++);
      }
    }

    return workingSet;
  }

  // Create a bucket sampler for O(1) random key selection
  BucketSampler createBucketSampler(const std::vector<AdId>& workingSet) const {
    return BucketSampler(distribution_, workingSet);
  }

 private:
  const std::vector<DistributionEntry>& distribution_;
};

// Benchmark class
class ChmBenchmark {
 public:
  struct BenchmarkConfig {
    int numThreads;
    int durationSeconds;
    int initialCapacity;
    bool verbose;
  };

  struct BenchmarkResults {
    Duration insertionTime;
    Duration benchmarkTime;
    uint64_t totalOperations;
    uint64_t successfulOperations;
    double operationsPerSecond;
    double successRate;
  };

  ChmBenchmark(
      const std::vector<DistributionEntry>& distribution,
      const BenchmarkConfig& config)
      : distribution_(distribution),
        config_(config),
        workloadGenerator_(distribution) {}

  // run: Execute the complete benchmark workflow
  // 1. Generate working set based on distribution
  // 2. Insert keys into ConcurrentHashMap using multiple threads
  // 3. Create bucket sampler for random key selection
  // 4. Run worker threads to measure getValue performance
  // 5. Calculate and return benchmark results
  BenchmarkResults run() {
    BenchmarkResults results{};

    // Calculate working set size
    int64_t workingSetSize = workloadGenerator_.calculateWorkingSetSize();
    if (config_.verbose) {
      std::cout << "Working set size: " << workingSetSize << " keys"
                << std::endl;
    }

    // Generate working set
    std::vector<AdId> workingSet = workloadGenerator_.generateWorkingSet();

    // Create and populate the ConcurrentHashMap
    ConcurrentHashMapType map(config_.initialCapacity);

    std::cout << "Inserting " << workingSet.size()
              << " keys into the map using " << config_.numThreads
              << " threads..." << std::endl;
    auto startInsert = Clock::now();

    // Multi-threaded insertion phase
    // Divides keys evenly among threads for parallel insertion
    std::vector<std::thread> insertThreads;
    std::atomic<int64_t> insertedKeys(0);

    // Calculate keys per thread
    size_t totalKeys = workingSet.size();
    size_t keysPerThread = totalKeys / config_.numThreads;
    size_t remainingKeys = totalKeys % config_.numThreads;

    // Launch insertion threads
    for (int i = 0; i < config_.numThreads; ++i) {
      size_t startIdx =
          i * keysPerThread + std::min(static_cast<size_t>(i), remainingKeys);
      size_t endIdx = startIdx + keysPerThread + (i < remainingKeys ? 1 : 0);

      insertThreads.emplace_back(
          [&map, &workingSet, &insertedKeys, startIdx, endIdx]() {
            uint64_t localInsertedKeys = 0;
            for (size_t j = startIdx; j < endIdx; ++j) {
              map.put(workingSet[j], std::make_shared<AdData>(workingSet[j]));
              localInsertedKeys++;

              if (localInsertedKeys % 10000 == 0) {
                insertedKeys.fetch_add(
                    localInsertedKeys, std::memory_order_relaxed);
                localInsertedKeys = 0;
              }
            }

            insertedKeys.fetch_add(
                localInsertedKeys, std::memory_order_relaxed);
          });
    }

    while (insertedKeys.load(std::memory_order_relaxed) < totalKeys) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      double progress =
          100.0 * insertedKeys.load(std::memory_order_relaxed) / totalKeys;
      std::cout << "\rInsertion progress: " << std::fixed
                << std::setprecision(1) << progress << "%" << std::flush;
    }
    std::cout << std::endl;

    // Wait for all insertion threads to complete
    for (auto& thread : insertThreads) {
      thread.join();
    }

    auto endInsert = Clock::now();
    results.insertionTime =
        std::chrono::duration_cast<Duration>(endInsert - startInsert);
    std::cout << "Insertion completed in " << results.insertionTime.count()
              << " ms" << std::endl;

    // Create bucket sampler for access distribution
    auto bucketSampler = workloadGenerator_.createBucketSampler(workingSet);

    std::cout << "Bucket sampler created with " << bucketSampler.numBuckets()
              << " buckets and " << bucketSampler.distributionSize()
              << " distribution entries" << std::endl;

    // Prepare for benchmark execution phase
    // Sets up worker threads to continuously call getValue on random keys
    bool shouldStop(false);
    std::atomic<uint64_t> totalOps(0);
    std::atomic<uint64_t> successfulOps(0);
    std::vector<std::thread> threads;

    std::cout << "Starting benchmark with " << config_.numThreads
              << " threads for " << config_.durationSeconds << " seconds..."
              << std::endl;

    // Start worker threads
    auto startBenchmark = Clock::now();

    for (int i = 0; i < config_.numThreads; ++i) {
      threads.emplace_back(
          &ChmBenchmark::workerThread,
          this,
          i,
          std::ref(map),
          std::ref(bucketSampler),
          std::ref(shouldStop),
          std::ref(totalOps),
          std::ref(successfulOps));
    }

    // Sleep for the specified duration
    std::this_thread::sleep_for(std::chrono::seconds(config_.durationSeconds));

    // Stop the benchmark
    shouldStop = true;

    // Wait for all threads to finish
    for (auto& thread : threads) {
      thread.join();
    }

    auto endBenchmark = Clock::now();
    results.benchmarkTime =
        std::chrono::duration_cast<Duration>(endBenchmark - startBenchmark);

    // Calculate results
    results.totalOperations = totalOps.load();
    results.successfulOperations = successfulOps.load();
    double durationSeconds = results.benchmarkTime.count() / 1000.0;
    results.operationsPerSecond =
        results.totalOperations / 1000000 / durationSeconds;
    results.successRate = static_cast<double>(results.successfulOperations) /
        results.totalOperations * 100.0;

    return results;
  }

  static void printResults(const BenchmarkResults& results) {
    std::cout << "\nBenchmark Results:" << std::endl;
    std::cout << "----------------" << std::endl;
    std::cout << "Duration: " << results.benchmarkTime.count() / 1000.0
              << " seconds" << std::endl;
    std::cout << "Total Operations: " << results.totalOperations << std::endl;
    std::cout << "Millions of Operations per Second: "
              << results.operationsPerSecond << " Mops/sec" << std::endl;
  }

 private:
  // workerThread: Core benchmark function executed by each thread
  // Continuously selects random keys and calls getValue until benchmark
  // completes Uses thread-local counters to minimize atomic operations
  void workerThread(
      int threadId,
      const ConcurrentHashMapType& map,
      const WorkloadGenerator::BucketSampler& bucketSampler,
      bool& shouldStop,
      std::atomic<uint64_t>& totalOps,
      std::atomic<uint64_t>& successfulOps) const {
    // Random number generator with thread ID as seed
    std::mt19937 rng(threadId);

    uint64_t localOps = 0;
    uint64_t localSuccessfulOps = 0;

    while (!shouldStop) {
      // Select a random key using the bucket sampler in O(1) time
      AdId key = bucketSampler.getRandomKey(rng);

      // Get value from the map
      auto result = map.getValue(key);

      localOps++;
      if (result.second) {
        localSuccessfulOps++;
      }
    }

    // Update atomic counters
    totalOps.fetch_add(localOps, std::memory_order_relaxed);
    successfulOps.fetch_add(localSuccessfulOps, std::memory_order_relaxed);
  }

  const std::vector<DistributionEntry>& distribution_;
  BenchmarkConfig config_;
  WorkloadGenerator workloadGenerator_;
};

} // namespace chm_benchmark

int main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_distribution_file.empty()) {
    std::cerr
        << "Error: Distribution file path is required. Use --distribution_file=<path>"
        << std::endl;
    return 1;
  }

  // Read distribution from file
  chm_benchmark::DistributionReader reader(FLAGS_distribution_file);
  auto distributionOpt = reader.read();

  if (!distributionOpt || distributionOpt->empty()) {
    std::cerr << "Error: Distribution file is empty or invalid" << std::endl;
    return 1;
  }

  // Configure and run benchmark
  chm_benchmark::ChmBenchmark::BenchmarkConfig config{
      .numThreads = FLAGS_num_threads,
      .durationSeconds = FLAGS_duration_seconds,
      .initialCapacity = FLAGS_initial_capacity,
      .verbose = FLAGS_verbose};

  chm_benchmark::ChmBenchmark benchmark(*distributionOpt, config);
  auto results = benchmark.run();

  // Print results
  chm_benchmark::ChmBenchmark::printResults(results);

  return 0;
}
