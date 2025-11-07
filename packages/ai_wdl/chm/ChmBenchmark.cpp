/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * ConcurrentHashMap Benchmark
 *
 * Measures ConcurrentHashMap read performance with configurable
 * access patterns based on frequency distributions.
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
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "folly/synchronization/Baton.h"

#include "./ConcurrentHashMap.h"

// Command line flags
DEFINE_string(distribution_file, "", "Path to the distribution CSV file");
DEFINE_int32(num_threads, 4, "Number of worker threads per batch");
DEFINE_int32(num_batch_threads, 2, "Number of parallel batch threads");
DEFINE_int32(duration_seconds, 10, "Benchmark duration in seconds");
DEFINE_int32(initial_capacity, 0, "Initial hash map capacity hint");
DEFINE_int32(batch_size, 1000, "Operations per batch");
DEFINE_int32(hit_ratio, 40, "Desired hit ratio (0-100)");
DEFINE_bool(verbose, false, "Enable verbose output");

namespace chm_benchmark {

// Performance and memory optimization constants
constexpr uint64_t MAX_DISTRIBUTION_ENTRIES =
    10000000; // Cap for memory efficiency
constexpr int PROGRESS_UPDATE_INTERVAL = 10000; // Progress reporting frequency
constexpr int PROGRESS_SLEEP_MS = 100; // Progress monitoring interval

// Type definitions for clarity and maintainability
using AdId = int64_t;
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

// Represents frequency distribution data from CSV input
struct DistributionEntry {
  uint64_t frequency; // Access frequency weight
  uint64_t numKeys; // Number of keys in this frequency bucket
};

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
// ConcurrentHashMap with 8 shards, F14FastMap backend, and SharedMutex
using ConcurrentHashMapType = ConcurrentHashMap<
    AdId,
    AdDataPtr,
    8,
    folly::F14FastMap<AdId, AdDataPtr>,
    folly::SharedMutex>;

/**
 * Parses distribution file in CSV format (frequency,numKeys)
 * Each line represents a frequency bucket with access weight and key count
 */
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
      if (auto entry = parseLine(line)) {
        distribution.push_back(*entry);
      }
    }

    return distribution;
  }

 private:
  // Parse a single CSV line into a DistributionEntry
  std::optional<DistributionEntry> parseLine(const std::string& line) const {
    std::istringstream iss(line);
    std::string freqStr, numKeysStr;

    if (std::getline(iss, freqStr, ',') && std::getline(iss, numKeysStr)) {
      try {
        return DistributionEntry{
            .frequency = static_cast<uint64_t>(std::stoll(freqStr)),
            .numKeys = static_cast<uint64_t>(std::stoll(numKeysStr))};
      } catch (const std::exception& e) {
        std::cerr << "Error parsing line: " << line << " - " << e.what()
                  << std::endl;
      }
    }
    return std::nullopt;
  }

  std::string filePath_;
};

/**
 * Generates workloads based on frequency distribution patterns
 * Creates realistic access patterns for benchmark testing
 */
class WorkloadGenerator {
 public:
  /**
   * Memory-optimized sampler for O(1) random key selection
   * Pre-builds distribution arrays to avoid runtime computation overhead
   */
  class BucketSampler {
   public:
    // Represents a frequency bucket with keys range
    struct Bucket {
      size_t startIndex; // Start index in shuffled working set
      uint64_t numKeys; // Number of keys in this bucket
    };

    BucketSampler(
        const std::vector<DistributionEntry>& distribution,
        const std::vector<AdId>& workingSet)
        : shuffledWorkingSet_(workingSet) {
      initializeBuckets(distribution);
      buildAccessDistribution(distribution);
    }

    // Pre-generate keys for a batch to minimize runtime overhead
    void preGenerateKeys(
        std::mt19937& rng,
        int hitRatio,
        std::vector<AdId>& keys) const {
      for (size_t i = 0; i < keys.size(); ++i) {
        bool shouldBeHit = (rng() % 100) < hitRatio;
        size_t bucketIdx =
            accessDistribution_[rng() % accessDistribution_.size()];
        const Bucket& bucket = buckets_[bucketIdx];
        uint64_t keyOffset = rng() % bucket.numKeys;

        if (shouldBeHit) {
          keys[i] = shuffledWorkingSet_[bucket.startIndex + keyOffset];
        } else {
          // Negative key indicates a miss (key not in map)
          keys[i] = -shuffledWorkingSet_[bucket.startIndex + keyOffset];
        }
      }
    }

    size_t numBuckets() const {
      return buckets_.size();
    }
    size_t distributionSize() const {
      return accessDistribution_.size();
    }

   private:
    // Initialize buckets and shuffle working set to avoid cache patterns
    void initializeBuckets(const std::vector<DistributionEntry>& distribution) {
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(shuffledWorkingSet_.begin(), shuffledWorkingSet_.end(), g);

      buckets_.reserve(distribution.size());
      size_t currentIndex = 0;

      for (const auto& entry : distribution) {
        buckets_.push_back({currentIndex, entry.numKeys});
        currentIndex += entry.numKeys;
      }
    }

    // Build pre-computed access distribution for O(1) bucket selection
    void buildAccessDistribution(
        const std::vector<DistributionEntry>& distribution) {
      uint64_t totalFrequency = 0;
      for (const auto& entry : distribution) {
        totalFrequency += entry.frequency * entry.numKeys;
      }

      // Scale down if needed to limit memory usage
      uint64_t totalEntries =
          std::min(totalFrequency, MAX_DISTRIBUTION_ENTRIES);
      double scalingFactor = static_cast<double>(totalEntries) / totalFrequency;

      accessDistribution_.reserve(totalEntries);

      // Fill distribution array based on frequency weights
      for (size_t i = 0; i < buckets_.size(); ++i) {
        const auto& entry = distribution[i];
        uint64_t numAppearances = std::max(
            static_cast<uint64_t>(
                entry.frequency * entry.numKeys * scalingFactor),
            uint64_t(1));

        for (uint64_t j = 0; j < numAppearances; ++j) {
          accessDistribution_.push_back(i);
        }
      }

      // Shuffle to avoid access patterns
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(accessDistribution_.begin(), accessDistribution_.end(), g);
    }

    std::vector<Bucket> buckets_;
    std::vector<size_t> accessDistribution_; // Pre-computed bucket selection
    std::vector<AdId> shuffledWorkingSet_; // Randomized key ordering
  };

  explicit WorkloadGenerator(const std::vector<DistributionEntry>& distribution)
      : distribution_(distribution) {}

  // Calculate total number of keys across all frequency buckets
  uint64_t calculateWorkingSetSize() const {
    uint64_t totalKeys = 0;
    for (const auto& entry : distribution_) {
      totalKeys += entry.numKeys;
    }
    return totalKeys;
  }

  // Calculate total weighted frequency for distribution normalization
  uint64_t calculateTotalFrequency() const {
    uint64_t totalFrequency = 0;
    for (const auto& entry : distribution_) {
      totalFrequency += entry.frequency * entry.numKeys;
    }
    return totalFrequency;
  }

  // Generate sequential key IDs for the working set
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

/**
 * Main benchmark orchestrator class
 * Coordinates setup, insertion, and benchmark execution phases
 */
class ChmBenchmark {
 public:
  // Configuration parameters for benchmark execution
  struct BenchmarkConfig {
    int numThreads; // Worker threads per batch
    int numBatchThreads; // Parallel batch processing threads
    int durationSeconds; // Benchmark duration
    int initialCapacity; // Hash map initial capacity hint
    int batchSize; // Operations per batch
    int hitRatio; // Desired hit ratio (0-100)
    bool verbose; // Enable detailed output
  };

  // Benchmark execution results and metrics
  struct BenchmarkResults {
    Duration insertionTime; // Time to populate hash map
    Duration benchmarkTime; // Total benchmark duration
    Duration workerThreadTime; // Actual worker thread execution time
    uint64_t totalOperations; // Total operations performed
    uint64_t successfulOperations; // Operations that found keys
    double operationsPerSecond; // Throughput in millions of ops/sec
    double successRate; // Percentage of successful operations
  };

  ChmBenchmark(
      const std::vector<DistributionEntry>& distribution,
      const BenchmarkConfig& config)
      : distribution_(distribution),
        config_(config),
        workloadGenerator_(distribution) {}

  BenchmarkResults run() {
    BenchmarkResults results{};

    // Setup phase
    auto workingSet = setupWorkingSet();
    ConcurrentHashMapType map(config_.initialCapacity);

    // Insertion phase
    results.insertionTime = insertKeys(map, workingSet);

    // Benchmark phase
    auto bucketSampler = workloadGenerator_.createBucketSampler(workingSet);
    results = runBenchmark(map, bucketSampler, results);

    return results;
  }

 private:
  std::vector<AdId> setupWorkingSet() {
    int64_t workingSetSize = workloadGenerator_.calculateWorkingSetSize();
    if (config_.verbose) {
      std::cout << "Working set size: " << workingSetSize << " keys"
                << std::endl;
    }
    return workloadGenerator_.generateWorkingSet();
  }

  Duration insertKeys(
      ConcurrentHashMapType& map,
      const std::vector<AdId>& workingSet) {
    std::cout << "Inserting " << workingSet.size()
              << " keys into the map using " << config_.numThreads
              << " threads..." << std::endl;

    auto startInsert = Clock::now();
    std::atomic<int64_t> insertedKeys(0);
    std::vector<std::thread> insertThreads;

    size_t totalKeys = workingSet.size();
    size_t keysPerThread = totalKeys / config_.numThreads;
    size_t remainingKeys = totalKeys % config_.numThreads;

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

              if (localInsertedKeys % PROGRESS_UPDATE_INTERVAL == 0) {
                insertedKeys.fetch_add(
                    localInsertedKeys, std::memory_order_relaxed);
                localInsertedKeys = 0;
              }
            }
            insertedKeys.fetch_add(
                localInsertedKeys, std::memory_order_relaxed);
          });
    }

    // Progress monitoring
    while (insertedKeys.load(std::memory_order_relaxed) < totalKeys) {
      std::this_thread::sleep_for(std::chrono::milliseconds(PROGRESS_SLEEP_MS));
      double progress =
          100.0 * insertedKeys.load(std::memory_order_relaxed) / totalKeys;
      std::cout << "\rInsertion progress: " << std::fixed
                << std::setprecision(1) << progress << "%" << std::flush;
    }
    std::cout << std::endl;

    for (auto& thread : insertThreads) {
      thread.join();
    }

    auto endInsert = Clock::now();
    auto insertionTime =
        std::chrono::duration_cast<Duration>(endInsert - startInsert);
    std::cout << "Insertion completed in " << insertionTime.count() << " ms"
              << std::endl;

    return insertionTime;
  }

  BenchmarkResults runBenchmark(
      const ConcurrentHashMapType& map,
      const WorkloadGenerator::BucketSampler& bucketSampler,
      BenchmarkResults results) {
    std::cout << "Bucket sampler created with " << bucketSampler.numBuckets()
              << " buckets and " << bucketSampler.distributionSize()
              << " distribution entries" << std::endl;

    std::atomic<uint64_t> totalOps(0);
    std::atomic<uint64_t> successfulOps(0);
    std::atomic<uint64_t> totalWorkerTimeNs(0);

    std::cout << "Starting benchmark with " << config_.numBatchThreads
              << " batch processing threads and " << config_.numThreads
              << " worker threads per batch for " << config_.durationSeconds
              << " seconds..." << std::endl;

    auto startBenchmark = Clock::now();
    auto endTime =
        startBenchmark + std::chrono::seconds(config_.durationSeconds);

    std::vector<std::thread> batchThreads;
    std::atomic<bool> shouldStop(false);

    for (int i = 0; i < config_.numBatchThreads; ++i) {
      batchThreads.emplace_back([this,
                                 i,
                                 &map,
                                 &bucketSampler,
                                 &totalOps,
                                 &successfulOps,
                                 &totalWorkerTimeNs,
                                 endTime,
                                 &shouldStop]() {
        std::mt19937 rng(i + 1000);
        std::vector<AdId> preGeneratedKeys(config_.batchSize);
        std::vector<AdId> nextBatchKeys(config_.batchSize);
        bucketSampler.preGenerateKeys(rng, config_.hitRatio, preGeneratedKeys);

        while (!shouldStop.load(std::memory_order_relaxed) &&
               Clock::now() < endTime) {
          processBatch(
              i,
              map,
              bucketSampler,
              totalOps,
              successfulOps,
              totalWorkerTimeNs,
              preGeneratedKeys,
              nextBatchKeys,
              rng);
          preGeneratedKeys.swap(nextBatchKeys);
        }
      });
    }

    std::this_thread::sleep_for(std::chrono::seconds(config_.durationSeconds));
    shouldStop.store(true, std::memory_order_relaxed);

    for (auto& thread : batchThreads) {
      thread.join();
    }

    auto endBenchmark = Clock::now();
    results.benchmarkTime =
        std::chrono::duration_cast<Duration>(endBenchmark - startBenchmark);
    results.totalOperations = totalOps.load();
    results.successfulOperations = successfulOps.load();
    results.workerThreadTime =
        std::chrono::milliseconds(totalWorkerTimeNs.load() / 1000000);

    double workerDurationSeconds = results.workerThreadTime.count() / 1000.0;
    results.operationsPerSecond =
        results.totalOperations / 1000000 / workerDurationSeconds;
    results.successRate = static_cast<double>(results.successfulOperations) /
        results.totalOperations * 100.0;

    return results;
  }

 public:
  // Display formatted benchmark results
  static void printResults(const BenchmarkResults& results) {
    std::cout << "\nBenchmark Results:" << std::endl;
    std::cout << "----------------" << std::endl;
    std::cout << "Total Benchmark Duration: "
              << results.benchmarkTime.count() / 1000.0 << " seconds"
              << std::endl;
    std::cout << "Worker Thread Time Only: "
              << results.workerThreadTime.count() / 1000.0 << " seconds"
              << std::endl;
    std::cout << "Total Operations: " << results.totalOperations << std::endl;
    std::cout << "Millions of Operations per Second: "
              << results.operationsPerSecond << " Mops/sec" << std::endl;
  }

 private:
  // Get thread pool executor for parallel batch processing
  folly::Executor* getPreprocessRequestExecutor() const {
    static folly::CPUThreadPoolExecutor cpuExecutor(FLAGS_num_threads);
    return &cpuExecutor;
  }

  /**
   * Process a single batch of operations with pre-generated keys
   * Overlaps next batch key generation with current batch execution
   */
  void processBatch(
      int batchThreadId,
      const ConcurrentHashMapType& map,
      const WorkloadGenerator::BucketSampler& bucketSampler,
      std::atomic<uint64_t>& totalOps,
      std::atomic<uint64_t>& successfulOps,
      std::atomic<uint64_t>& totalWorkerTimeNs,
      const std::vector<AdId>& preGeneratedKeys,
      std::vector<AdId>& nextBatchKeys,
      std::mt19937& rng) const {
    // Synchronization barrier for batch completion
    folly::Baton<> baton;
    std::shared_ptr<folly::Baton<>> batonGuard(
        &baton, [](folly::Baton<>* baton) { baton->post(); });

    const int opsPerThread = config_.batchSize / config_.numThreads;

    // Launch worker threads for current batch
    for (int i = 0; i < config_.numThreads; ++i) {
      getPreprocessRequestExecutor()->add(
          [&, batonGuard, i, batchThreadId]() mutable {
            const int start = i * opsPerThread;
            int end = (i + 1) * opsPerThread;
            if (i == (config_.numThreads - 1)) {
              end = config_.batchSize; // Handle remainder operations
            }

            int uniqueWorkerId = batchThreadId * config_.numThreads + i;

            // Time worker thread execution for accurate throughput calculation
            auto workerStart = std::chrono::high_resolution_clock::now();

            workerThread(
                uniqueWorkerId,
                start,
                end,
                map,
                preGeneratedKeys,
                totalOps,
                successfulOps);

            auto workerEnd = std::chrono::high_resolution_clock::now();
            auto workerDuration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    workerEnd - workerStart);

            totalWorkerTimeNs.fetch_add(
                workerDuration.count(), std::memory_order_relaxed);
          });
    }

    // Generate keys for next batch while current batch executes (latency
    // hiding)
    bucketSampler.preGenerateKeys(rng, config_.hitRatio, nextBatchKeys);

    // Wait for batch completion
    batonGuard.reset();
    baton.wait();
  }

  /**
   * Core benchmark worker function
   * Performs hash map lookups using pre-generated keys
   */
  void workerThread(
      int threadId,
      int startIdx,
      int endIdx,
      const ConcurrentHashMapType& map,
      const std::vector<AdId>& preGeneratedKeys,
      std::atomic<uint64_t>& totalOps,
      std::atomic<uint64_t>& successfulOps) const {
    uint64_t localOps = 0;
    uint64_t localSuccessfulOps = 0;

    // Process assigned range of operations
    for (int i = startIdx; i < endIdx; i++) {
      AdId key = preGeneratedKeys[i];
      auto result = map.getValue(key); // Core benchmark operation

      localOps++;
      if (result.second) { // Check if key was found
        localSuccessfulOps++;
      }
    }

    // Batch update atomic counters to reduce contention
    totalOps.fetch_add(localOps, std::memory_order_relaxed);
    successfulOps.fetch_add(localSuccessfulOps, std::memory_order_relaxed);
  }

  const std::vector<DistributionEntry>& distribution_;
  BenchmarkConfig config_;
  WorkloadGenerator workloadGenerator_;
};

} // namespace chm_benchmark

/**
 * Main entry point for the ConcurrentHashMap benchmark
 * Parses command line arguments, loads distribution data, and executes
 * benchmark
 */
int main(int argc, char* argv[]) {
  // Parse command line flags
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_distribution_file.empty()) {
    std::cerr
        << "Error: Distribution file path is required. Use --distribution_file=<path>"
        << std::endl;
    return 1;
  }

  // Load frequency distribution data from CSV file
  chm_benchmark::DistributionReader reader(FLAGS_distribution_file);
  auto distributionOpt = reader.read();

  if (!distributionOpt || distributionOpt->empty()) {
    std::cerr << "Error: Distribution file is empty or invalid" << std::endl;
    return 1;
  }

  // Configure benchmark parameters from command line flags
  chm_benchmark::ChmBenchmark::BenchmarkConfig config{
      .numThreads = FLAGS_num_threads,
      .numBatchThreads = FLAGS_num_batch_threads,
      .durationSeconds = FLAGS_duration_seconds,
      .initialCapacity = FLAGS_initial_capacity,
      .batchSize = FLAGS_batch_size,
      .hitRatio = FLAGS_hit_ratio,
      .verbose = FLAGS_verbose};

  // Execute benchmark and display results
  chm_benchmark::ChmBenchmark benchmark(*distributionOpt, config);
  auto results = benchmark.run();
  chm_benchmark::ChmBenchmark::printResults(results);

  return 0;
}
