/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * Deserialization Benchmark (DeserBench)
 *
 * A benchmark for measuring Ideserialization performance
 * with configurable access patterns based on frequency distributions.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <folly/FileUtil.h>
#include <folly/Format.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/io/IOBuf.h>
#include <gflags/gflags.h>

// Define command line flags
DEFINE_string(distribution_file, "", "Path to the distribution CSV file");
DEFINE_int32(num_threads, 4, "Number of threads for benchmark");
DEFINE_int32(duration_seconds, 10, "Duration of the benchmark in seconds");
DEFINE_int32(
    pregenerated_copies,
    1,
    "Number of copies to pre-generate for each IOBuf configuration");

namespace deser_benchmark {

// Type definitions
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::milliseconds;

/**
 * Utility functions
 */
namespace util {

/**
 * Formats a byte size into a human-readable string (B, KB, MB, GB)
 */
std::string formatByteSize(size_t bytes) {
  constexpr double KB = 1024.0;
  constexpr double MB = KB * 1024.0;
  constexpr double GB = MB * 1024.0;

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2);

  if (bytes < KB) {
    oss << bytes << " bytes";
  } else if (bytes < MB) {
    oss << (bytes / KB) << " KB";
  } else if (bytes < GB) {
    oss << (bytes / MB) << " MB";
  } else {
    oss << (bytes / GB) << " GB";
  }

  return oss.str();
}

} // namespace util

/**
 * Function to mimic loadTensor which takes an IOBuf as input
 * Copies the IOBuf and calls coalesceWithHeadroomTailroom
 */

folly::IOBuf
loadTensor(const folly::IOBuf& input, size_t headroom, size_t tailroom) {
  folly::IOBuf data = input;
  data.coalesceWithHeadroomTailroom(headroom, tailroom);

  return data;
}

/**
 * Structure to hold IOBuf chain description from the distribution file
 */
struct IOBufChainDesc {
  int frequency;
  size_t headroom;
  size_t tailroom;
  std::vector<std::tuple<size_t, size_t, size_t>> chains; // (h, d, t)
};

/**
 * Structure to hold a pregenerated IOBuf and its associated parameters
 */
struct IOBufInstance {
  std::unique_ptr<folly::IOBuf> buf;
  size_t headroom;
  size_t tailroom;
};

/**
 * DistributionReader: Parses the distribution file into IOBufChainDesc objects
 * Format: frequency,headroom,tailroom,chain_description
 */
class DistributionReader {
 public:
  explicit DistributionReader(std::string_view filePath)
      : filePath_(filePath) {}

  std::optional<std::vector<IOBufChainDesc>> read() const {
    std::vector<IOBufChainDesc> distribution;
    std::string content;

    // Read the file content
    if (!folly::readFile(filePath_.data(), content)) {
      std::cerr << "Error: Could not open distribution file: " << filePath_
                << std::endl;
      return std::nullopt;
    }

    std::vector<folly::StringPiece> lines;
    folly::split('\n', content, lines);

    for (const auto& line : lines) {
      if (line.empty()) {
        continue;
      }

      auto desc = parseLine(line);
      if (desc) {
        distribution.push_back(*desc);
      }
    }

    return distribution;
  }

 private:
  std::optional<IOBufChainDesc> parseLine(
      const folly::StringPiece& line) const {
    std::vector<folly::StringPiece> columns;
    folly::split(',', line, columns);

    if (columns.size() != 4) {
      std::cerr << "Warning: Skipping invalid line (expected 4 columns): "
                << line << std::endl;
      return std::nullopt;
    }

    try {
      IOBufChainDesc desc;
      desc.frequency = folly::to<int>(columns[0]);
      desc.headroom = folly::to<size_t>(columns[1]);
      desc.tailroom = folly::to<size_t>(columns[2]);

      // Parse the chain description
      if (!parseChainDescription(columns[3].str(), desc.chains)) {
        std::cerr << "Warning: No valid chains found in line: " << line
                  << std::endl;
        return std::nullopt;
      }

      return desc;
    } catch (const std::exception& e) {
      std::cerr << "Error parsing line: " << line << " - " << e.what()
                << std::endl;
      return std::nullopt;
    }
  }

  bool parseChainDescription(
      const std::string& chainStr,
      std::vector<std::tuple<size_t, size_t, size_t>>& chains) const {
    std::regex chainRegex(R"(\{h:(\d+)\|d:(\d+)\|t:(\d+)\})");

    auto begin =
        std::sregex_iterator(chainStr.begin(), chainStr.end(), chainRegex);
    auto end = std::sregex_iterator();

    for (std::sregex_iterator i = begin; i != end; ++i) {
      std::smatch match = *i;
      size_t h = std::stoul(match[1].str());
      size_t d = std::stoul(match[2].str());
      size_t t = std::stoul(match[3].str());
      chains.emplace_back(h, d, t);
    }

    return !chains.empty();
  }

  std::string_view filePath_;
};

/**
 * Workload generator class
 * Responsible for creating IOBuf instances based on the distribution
 */
class WorkloadGenerator {
 public:
  /**
   * IOBufSampler: Fast sampler for frequency-based random IOBuf selection
   * Uses O(1) lookup for maximum performance at the cost of higher memory usage
   */
  class IOBufSampler {
   public:
    explicit IOBufSampler(const std::vector<IOBufChainDesc>& distribution) {
      if (distribution.empty()) {
        return;
      }

      // Calculate total frequency and determine if we need to cap the
      // distribution size
      uint64_t totalFrequency = 0;
      for (const auto& entry : distribution) {
        totalFrequency += entry.frequency;
      }

      // Cap the distribution size to avoid excessive memory usage
      constexpr uint64_t maxEntries = 10000000; // 10 million entries max
      uint64_t distributionSize = std::min(totalFrequency, maxEntries);

      // Calculate scaling factor if needed
      double scalingFactor =
          static_cast<double>(distributionSize) / totalFrequency;

      // Pre-allocate the distribution array for better performance
      accessDistribution_.reserve(distributionSize);

      // Fill the distribution array with indices proportional to their
      // frequency
      for (size_t i = 0; i < distribution.size(); ++i) {
        const auto& entry = distribution[i];

        // Calculate how many times this entry should appear in the array
        uint64_t appearances =
            static_cast<uint64_t>(entry.frequency * scalingFactor);

        // Ensure each entry appears at least once if it has non-zero frequency
        if (entry.frequency > 0 && appearances == 0) {
          appearances = 1;
        }

        // Add the index to the distribution array multiple times
        for (uint64_t j = 0; j < appearances; ++j) {
          accessDistribution_.push_back(i);
        }
      }

      // Shuffle the distribution array to avoid patterns
      std::random_device rd;
      std::mt19937 g(rd());
      std::shuffle(accessDistribution_.begin(), accessDistribution_.end(), g);

      // Store the distribution for later use
      distribution_ = distribution;

      if (accessDistribution_.empty()) {
        std::cerr << "Warning: Generated empty access distribution"
                  << std::endl;
      }
    }

    /**
     * Get a random IOBuf chain description based on the frequency distribution
     * Uses O(1) lookup for maximum performance
     *
     * @param rng Random number generator
     * @return Reference to a randomly selected IOBufChainDesc
     */
    const IOBufChainDesc& getRandomIOBufDesc(std::mt19937& rng) const {
      if (distribution_.empty() || accessDistribution_.empty()) {
        static IOBufChainDesc emptyDesc{};
        return emptyDesc;
      }

      size_t idx = getRandomIndex(rng);
      return distribution_[idx];
    }

    /**
     * Get a random index into the distribution array
     */
    size_t getRandomIOBufIndex(std::mt19937& rng) const {
      if (distribution_.empty() || accessDistribution_.empty()) {
        return 0;
      }

      return getRandomIndex(rng);
    }

    /**
     * Get the number of unique IOBuf chain descriptions
     */
    size_t numDescriptions() const {
      return distribution_.size();
    }

    /**
     * Get the size of the access distribution array
     */
    size_t distributionSize() const {
      return accessDistribution_.size();
    }

   private:
    /**
     * Helper method to get a random index based on frequency weights
     * O(1) time complexity
     */
    size_t getRandomIndex(std::mt19937& rng) const {
      // Generate a random index into the access distribution array
      size_t randomIdx = rng() % accessDistribution_.size();

      // Return the configuration index stored at that position
      return accessDistribution_[randomIdx];
    }

    std::vector<size_t>
        accessDistribution_; // Pre-generated distribution array for O(1) lookup
    std::vector<IOBufChainDesc> distribution_; // Copy of the distribution
  };

  explicit WorkloadGenerator(const std::vector<IOBufChainDesc>& distribution)
      : distribution_(distribution) {}

  /**
   * Create an IOBuf sampler for random IOBuf selection
   */
  IOBufSampler createIOBufSampler() const {
    return IOBufSampler(distribution_);
  }

  /**
   * Generate an IOBuf chain based on the description
   */
  static std::unique_ptr<folly::IOBuf> generateIOBufChain(
      const IOBufChainDesc& desc) {
    std::unique_ptr<folly::IOBuf> head;
    folly::IOBuf* current = nullptr;

    for (const auto& chain : desc.chains) {
      size_t h = std::get<0>(chain);
      size_t d = std::get<1>(chain);
      size_t t = std::get<2>(chain);

      auto buf = folly::IOBuf::create(h + d + t);
      buf->reserve(0, h); // Reserve headroom
      buf->append(d); // Append data size

      // Fill with some pattern data
      memset(buf->writableData(), 'A', d);

      if (!head) {
        head = std::move(buf);
        current = head.get();
      } else {
        current->appendChain(std::move(buf));
        current = current->next();
      }
    }

    assert(head);

    return head;
  }

  /**
   * Pregenerate IOBuf instances for all configurations
   */
  std::vector<std::vector<IOBufInstance>> pregenerateIOBufs(
      int numCopiesPerConfig) const {
    if (distribution_.empty() || numCopiesPerConfig <= 0) {
      std::cerr << "Warning: Empty distribution or invalid copy count"
                << std::endl;
      return {};
    }

    std::vector<std::vector<IOBufInstance>> result;
    result.reserve(distribution_.size());

    std::cout << "Pregenerating IOBufs for " << distribution_.size()
              << " unique configurations..." << std::endl;

    PregenerationStats stats =
        generateIOBufInstances(result, numCopiesPerConfig);
    printPregenerationStats(stats, numCopiesPerConfig);

    return result;
  }

 private:
  /**
   * Structure to hold pregeneration statistics
   */
  struct PregenerationStats {
    size_t totalConfigurations{0};
    size_t totalInstances{0};
    size_t totalMemoryBytes{0};
  };

  /**
   * Calculate the total memory size of an IOBuf chain
   * This includes the capacity of all buffers in the chain (headroom + data +
   * tailroom)
   *
   * @param buf The IOBuf chain to calculate the size for
   * @return The total memory size in bytes
   */
  static size_t calculateIOBufSize(const folly::IOBuf& buf) {
    size_t totalSize = 0;

    // Get the first buffer in the chain
    const folly::IOBuf* current = &buf;

    // Iterate through the chain manually
    do {
      // Add capacity (total allocated memory including headroom and tailroom)
      totalSize += current->capacity();
      current = current->next();
    } while (current != &buf);

    return totalSize;
  }

  /**
   * Generate IOBuf instances and collect statistics
   *
   * @param result Vector to store the generated IOBuf instances
   * @param numCopiesPerConfig Number of copies to generate for each
   * configuration
   * @return Statistics about the generated instances
   */
  PregenerationStats generateIOBufInstances(
      std::vector<std::vector<IOBufInstance>>& result,
      int numCopiesPerConfig) const {
    PregenerationStats stats;

    for (const auto& desc : distribution_) {
      std::vector<IOBufInstance> instances;
      instances.reserve(numCopiesPerConfig);
      stats.totalConfigurations++;

      for (int i = 0; i < numCopiesPerConfig; ++i) {
        IOBufInstance instance;
        instance.buf = generateIOBufChain(desc);
        instance.headroom = desc.headroom;
        instance.tailroom = desc.tailroom;

        // Calculate and add the size of this IOBuf chain
        stats.totalMemoryBytes += calculateIOBufSize(*instance.buf);
        stats.totalInstances++;

        instances.push_back(std::move(instance));
      }

      result.push_back(std::move(instances));
    }

    return stats;
  }

  /**
   * Print pregeneration statistics
   */
  void printPregenerationStats(
      const PregenerationStats& stats,
      int numCopiesPerConfig) const {
    std::cout << "Pregeneration complete:" << std::endl;
    std::cout << "- " << stats.totalConfigurations << " unique configurations"
              << std::endl;
    std::cout << "- " << stats.totalInstances << " total IOBuf instances ("
              << numCopiesPerConfig << " copies each)" << std::endl;
    std::cout << "- " << util::formatByteSize(stats.totalMemoryBytes)
              << " total memory usage" << std::endl;
  }

  const std::vector<IOBufChainDesc>& distribution_;
};

/**
 * Benchmark class
 * Responsible for running the benchmark and collecting results
 */
class DeserBenchmark {
 public:
  /**
   * Configuration for the benchmark
   */
  struct BenchmarkConfig {
    int numThreads;
    int durationSeconds;
    int pregeneratedCopies;
  };

  /**
   * Results from the benchmark
   */
  struct BenchmarkResults {
    Duration pregenerationTime;
    Duration benchmarkTime;
    uint64_t totalOperations;
    double operationsPerSecond;
  };

  DeserBenchmark(
      const std::vector<IOBufChainDesc>& distribution,
      const BenchmarkConfig& config)
      : distribution_(distribution),
        config_(config),
        workloadGenerator_(distribution) {}

  /**
   * Run the complete benchmark workflow
   */
  BenchmarkResults run() {
    BenchmarkResults results{};

    // Create IOBuf sampler for access distribution
    auto iobufSampler = workloadGenerator_.createIOBufSampler();

    std::cout << "IOBuf sampler created with " << iobufSampler.numDescriptions()
              << " unique IOBuf descriptions and "
              << iobufSampler.distributionSize() << " distribution entries"
              << std::endl;

    // Pregenerate IOBuf instances
    auto pregeneratedIOBufs = pregenerateIOBufs(results);

    // Run the benchmark
    runBenchmark(pregeneratedIOBufs, iobufSampler, results);

    return results;
  }

  /**
   * Print benchmark results
   */
  static void printResults(const BenchmarkResults& results) {
    std::cout << "\nBenchmark Results:" << std::endl;
    std::cout << "----------------" << std::endl;
    std::cout << "Benchmark Duration: "
              << results.benchmarkTime.count() / 1000.0 << " seconds"
              << std::endl;
    std::cout << "Total Operations: " << results.totalOperations << std::endl;
    std::cout << "Millions of Operations per Second: "
              << results.operationsPerSecond << " Mops/sec" << std::endl;
  }

 private:
  /**
   * Pregenerate IOBuf instances and measure time
   */
  std::vector<std::vector<IOBufInstance>> pregenerateIOBufs(
      BenchmarkResults& results) {
    std::cout << "Pregenerating IOBufs..." << std::endl;
    auto pregeneratedIOBufs =
        workloadGenerator_.pregenerateIOBufs(config_.pregeneratedCopies);
    return pregeneratedIOBufs;
  }

  /**
   * Run the benchmark with the given IOBuf instances
   */
  void runBenchmark(
      const std::vector<std::vector<IOBufInstance>>& pregeneratedIOBufs,
      const WorkloadGenerator::IOBufSampler& iobufSampler,
      BenchmarkResults& results) {
    // Prepare for benchmark execution phase
    bool shouldStop(false);
    std::atomic<uint64_t> totalOps(0);
    std::vector<std::thread> threads;
    threads.reserve(config_.numThreads);

    std::cout << "Starting benchmark with " << config_.numThreads
              << " threads for " << config_.durationSeconds << " seconds..."
              << std::endl;

    // Start worker threads
    auto startBenchmark = Clock::now();

    for (int i = 0; i < config_.numThreads; ++i) {
      threads.emplace_back(
          &DeserBenchmark::workerThread,
          this,
          i,
          std::ref(iobufSampler),
          std::ref(pregeneratedIOBufs),
          std::ref(shouldStop),
          std::ref(totalOps));
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
    double durationSeconds = results.benchmarkTime.count() / 1000.0;
    results.operationsPerSecond =
        results.totalOperations / 1000000 / durationSeconds;
  }

  /**
   * Worker thread function executed by each thread
   *
   * @param threadId ID of the worker thread
   * @param iobufSampler Sampler for selecting IOBuf configurations
   * @param pregeneratedIOBufs Pregenerated IOBuf instances
   * @param shouldStop Flag to indicate when the thread should stop
   * @param totalOps Atomic counter for tracking total operations
   */
  void workerThread(
      int threadId,
      const WorkloadGenerator::IOBufSampler& iobufSampler,
      const std::vector<std::vector<IOBufInstance>>& pregeneratedIOBufs,
      bool& shouldStop,
      std::atomic<uint64_t>& totalOps) const {
    // Random number generator with thread ID as seed
    std::mt19937 rng(threadId);

    uint64_t localOps = 0;
    constexpr uint64_t batchSize = 10000;

    while (!shouldStop) {
      // Select a random IOBuf configuration index
      size_t configIdx = iobufSampler.getRandomIOBufIndex(rng);

      // Ensure the index is valid
      if (configIdx >= pregeneratedIOBufs.size()) {
        continue;
      }

      const auto& instances = pregeneratedIOBufs[configIdx];

      size_t instanceIdx = rng() % instances.size();

      // Get the selected IOBuf instance
      const auto& instance = instances[instanceIdx];

      // Call loadTensor with the pregenerated IOBuf
      volatile auto result =
          loadTensor(*instance.buf, instance.headroom, instance.tailroom);

      localOps++;

      // Update atomic counter periodically to reduce contention
      if (localOps % batchSize == 0) {
        totalOps.fetch_add(batchSize, std::memory_order_relaxed);
        localOps = 0;
      }
    }

    // Update atomic counter with remaining operations
    totalOps.fetch_add(localOps, std::memory_order_relaxed);
  }

  const std::vector<IOBufChainDesc>& distribution_;
  BenchmarkConfig config_;
  WorkloadGenerator workloadGenerator_;
};

} // namespace deser_benchmark

/**
 * Main function
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

  // Read distribution from file
  deser_benchmark::DistributionReader reader(FLAGS_distribution_file);
  auto distributionOpt = reader.read();

  if (!distributionOpt || distributionOpt->empty()) {
    std::cerr << "Error: Distribution file is empty or invalid" << std::endl;
    return 1;
  }

  // Validate command line arguments
  if (FLAGS_num_threads <= 0) {
    std::cerr << "Error: Number of threads must be positive" << std::endl;
    return 1;
  }

  if (FLAGS_duration_seconds <= 0) {
    std::cerr << "Error: Duration must be positive" << std::endl;
    return 1;
  }

  if (FLAGS_pregenerated_copies <= 0) {
    std::cerr << "Error: Number of pregenerated copies must be positive"
              << std::endl;
    return 1;
  }
  // Configure and run benchmark
  deser_benchmark::DeserBenchmark::BenchmarkConfig config{
      .numThreads = FLAGS_num_threads,
      .durationSeconds = FLAGS_duration_seconds,
      .pregeneratedCopies = FLAGS_pregenerated_copies};

  deser_benchmark::DeserBenchmark benchmark(*distributionOpt, config);
  auto results = benchmark.run();

  // Print results
  deser_benchmark::DeserBenchmark::printResults(results);

  return 0;
}
