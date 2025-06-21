/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// Size of the output tensor in bytes (will be overridden by command line)
size_t OUTPUT_TENSOR_SIZE = 2421002;

// Default prefetch distance (0 means no prefetching)
constexpr int DEFAULT_PREFETCH_DIST = 0;

/**
 * SizeDistribution - Efficient tensor size sampling using CDF approach
 *
 * Uses binary search on a cumulative distribution function (CDF) to efficiently
 * sample tensor sizes according to a specified probability distribution.
 */
struct SizeDistribution {
  std::vector<size_t> sizes; // Array of unique sizes
  std::vector<double>
      cum_probs; // Array of corresponding cumulative probabilities

  // Thread-local random number generator for better performance
  thread_local static std::mt19937 gen;

  /**
   * Load size distribution from a CSV file
   * Format: size,frequency where frequency is a percentage (0-100)
   * Example: 24460,0.69
   *
   * Builds a cumulative distribution function (CDF) for efficient sampling
   */
  bool loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open size distribution file: " << filename
                << std::endl;
      return false;
    }

    sizes.clear();
    cum_probs.clear();
    std::string line;
    std::vector<std::pair<size_t, double>> sizeFreqPairs;

    // Read size and frequency pairs from the file
    while (std::getline(file, line)) {
      std::istringstream iss(line);
      std::string sizeStr, freqStr;

      if (std::getline(iss, sizeStr, ',') && std::getline(iss, freqStr)) {
        try {
          size_t size = std::stoul(sizeStr);
          double freq = std::stod(freqStr);
          sizeFreqPairs.emplace_back(size, freq / 100);
        } catch (const std::exception& e) {
          std::cerr << "Error parsing line: " << line << " - " << e.what()
                    << std::endl;
        }
      }
    }

    // Sort by size for binary search efficiency
    std::sort(
        sizeFreqPairs.begin(),
        sizeFreqPairs.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Build cumulative distribution
    double cumProb = 0.0;
    for (const auto& pair : sizeFreqPairs) {
      sizes.push_back(pair.first);
      cumProb += pair.second;
      cum_probs.push_back(cumProb);
    }

    // Ensure the last cumulative probability is exactly 1.0
    if (!cum_probs.empty()) {
      cum_probs.back() = 1.0;
    }

    std::cout << "Loaded " << sizes.size()
              << " unique sizes with cumulative probability distribution"
              << std::endl;
    return true;
  }

  /**
   * Sample a random size using binary search on the CDF
   * Time complexity: O(log n) where n is the number of unique sizes
   * Uses thread-local RNG for better performance in multi-threaded scenarios
   */
  size_t getRandomSize() const {
    if (sizes.empty()) {
      return 0;
    }

    // Initialize thread-local random generator if needed
    static thread_local std::mt19937 local_gen(std::random_device{}());

    // Generate random value between 0 and 1
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double rand_val = dist(local_gen);

    // Binary search to find the corresponding size
    auto it = std::lower_bound(cum_probs.begin(), cum_probs.end(), rand_val);
    size_t index = std::distance(cum_probs.begin(), it);

    // Ensure index is within bounds
    if (index >= sizes.size()) {
      index = sizes.size() - 1;
    }

    return sizes[index];
  }
};

// Initialize the thread-local random generator
thread_local std::mt19937 SizeDistribution::gen{};

/**
 * InputMemoryPool - Large pre-allocated memory pool for input tensors
 *
 * Allocates a single large memory chunk (configurable size in GB) to simulate
 * a realistic memory environment. Tensors are positioned at random offsets
 * within this pool to create realistic memory access patterns.
 */
class InputMemoryPool {
 public:
  // Initialize with a large memory chunk (size in GB)
  InputMemoryPool(size_t size_gb) {
    // Convert GB to bytes
    size_t size_bytes = size_gb * 1024ULL * 1024ULL * 1024ULL;

    std::cout << "Allocating a large memory pool of " << size_gb << " GB ("
              << size_bytes << " bytes) for input tensors" << std::endl;

    // Allocate the memory chunk
    memory_ = malloc(size_bytes);
    if (!memory_) {
      throw std::bad_alloc();
    }

    // Initialize with some pattern for validation if needed
    // This is optional and can be removed if not needed
    memset(memory_, 0xAB, size_bytes);

    size_ = size_bytes;
  }

  ~InputMemoryPool() {
    if (memory_) {
      free(memory_);
      memory_ = nullptr;
    }
  }

  // Get a pointer at a specific offset in the memory pool
  void* getPointerAtOffset(size_t offset) const {
    if (offset < size_) {
      return static_cast<char*>(memory_) + offset;
    }
    return nullptr;
  }

  // Get the total size of the memory pool
  size_t getSize() const {
    return size_;
  }

 private:
  void* memory_ = nullptr;
  size_t size_ = 0;
};

// Global input memory pool
std::unique_ptr<InputMemoryPool> g_input_memory_pool;

/**
 * TensorBatch - Collection of tensors with random offsets in memory pool
 *
 * Creates a batch of tensors with:
 * 1. Sizes sampled from the provided distribution
 * 2. Random offsets within the large memory pool
 * 3. Total size limited by OUTPUT_TENSOR_SIZE
 */
class TensorBatch {
 public:
  TensorBatch(size_t count, const SizeDistribution& size_dist) {
    if (!g_input_memory_pool) {
      throw std::runtime_error("Input memory pool not initialized");
    }

    // Initialize random number generator for offsets
    static thread_local std::mt19937 rng(std::random_device{}());

    // Get the total size of the memory pool
    size_t pool_size = g_input_memory_pool->getSize();

    // Allocate memory for each tensor based on the size distribution
    for (size_t i = 0; i < count; ++i) {
      size_t size = size_dist.getRandomSize();
      if (size == 0)
        continue;

      // Check if adding this tensor would exceed the output tensor size
      if (total_size + size > OUTPUT_TENSOR_SIZE) {
        break;
      }

      // Calculate maximum possible offset to ensure we don't exceed the pool
      // size
      size_t max_offset = pool_size - size;

      // Generate a random offset within the valid range
      std::uniform_int_distribution<size_t> dist(0, max_offset);
      size_t offset = dist(rng);

      // Get a pointer at the random offset in the memory pool
      void* tensor = g_input_memory_pool->getPointerAtOffset(offset);

      if (tensor) {
        tensors.push_back(tensor);
        tensor_sizes.push_back(size);
        total_size += size;
      }
    }
  }

  ~TensorBatch() {
    // No need to free tensors - they're pre-allocated and managed globally
    tensors.clear();
    tensor_sizes.clear();
  }

  // Get the vector of tensor pointers
  const std::vector<const void*>& getTensors() const {
    return reinterpret_cast<const std::vector<const void*>&>(tensors);
  }

  // Get the size of a specific tensor
  size_t getTensorSize(size_t index) const {
    if (index < tensor_sizes.size()) {
      return tensor_sizes[index];
    }
    return 0;
  }

  // Get total number of tensors
  size_t getTensorCount() const {
    return tensors.size();
  }

  // Get total size of all tensors
  size_t getTotalSize() const {
    return total_size;
  }

 private:
  std::vector<void*> tensors;
  std::vector<size_t> tensor_sizes;
  size_t total_size = 0;
};

/**
 * RebatchBenchmark - Multi-threaded memory copy benchmark
 *
 * Key features:
 * 1. Online batch generation - Creates batches on-the-fly during benchmarking
 * 2. Fresh output tensors - Allocates new output tensor for each batch to
 * prevent cache effects
 * 3. Multi-threading - Supports configurable number of worker threads
 * 4. Prefetching - Optional memory prefetching for improved performance
 * 5. Anti-optimization - Uses checksums to prevent compiler from optimizing
 * away operations
 */
class RebatchBenchmark {
 public:
  RebatchBenchmark(
      size_t num_threads,
      size_t tensors_per_batch,
      const SizeDistribution& size_dist,
      int prefetch_dist = DEFAULT_PREFETCH_DIST)
      : num_threads_(num_threads),
        stop_flag_(false),
        tensors_per_batch_(tensors_per_batch),
        size_dist_(size_dist),
        prefetch_dist_(prefetch_dist) {
    std::cout
        << "Initialized benchmark with online batch generation and fresh output tensors"
        << std::endl;

    // Initialize statistics counters
    for (size_t i = 0; i < num_threads_; ++i) {
      bytes_processed_.push_back(0);
      batches_processed_.push_back(0);
    }
  }

  ~RebatchBenchmark() {
    // No output tensors to free - they're allocated and freed per batch
  }

  // Run the benchmark for the specified duration
  void run(std::chrono::seconds duration) {
    std::cout << "Starting benchmark with " << num_threads_ << " threads for "
              << duration.count() << " seconds (online batch generation)"
              << std::endl;

    // Record start time
    auto start_time = std::chrono::high_resolution_clock::now();

    // Create and start threads
    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_threads_; ++i) {
      threads.push_back(std::thread(&RebatchBenchmark::workerThread, this, i));
    }

    // Sleep for the specified duration
    std::this_thread::sleep_for(duration);

    // Signal threads to stop and wait for them
    stop_flag_ = true;
    for (auto& thread : threads) {
      thread.join();
    }

    // Record end time and calculate actual duration
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;

    // Print results
    printResults(elapsed_seconds.count());
  }

 private:
  /**
   * Worker thread function that processes batches
   *
   * Each thread:
   * 1. Allocates a fresh output tensor for each batch
   * 2. Generates a new batch with random tensor sizes and offsets
   * 3. Performs memcpy operations with optional prefetching
   * 5. Tracks performance statistics
   */
  void workerThread(size_t thread_id) {
    // Checksum to prevent compiler from optimizing away memcpy operations
    volatile uint64_t checksum = 0;

    while (!stop_flag_) {
      // Allocate a fresh output tensor for this batch
      char* output = new char[OUTPUT_TENSOR_SIZE];

      // Generate a new batch on-the-fly for each iteration.
      // This simulates real-world scenarios where batches are created
      // dynamically.
      auto batch =
          std::make_shared<TensorBatch>(tensors_per_batch_, size_dist_);
      const auto& tensors = batch->getTensors();
      const size_t tensorsSize = tensors.size();

      if (tensorsSize == 0) {
        continue; // Skip empty batches
      }

      // Prepare vectors for tensor pointers and sizes
      std::vector<const void*> allDataPtrs(tensorsSize);
      std::vector<size_t> allNbytes(tensorsSize);

      // Collect all tensor pointers and sizes
      for (size_t i = 0; i < tensorsSize; ++i) {
        allDataPtrs[i] = tensors[i];
        allNbytes[i] = batch->getTensorSize(i);
      }

      // Process all tensors sequentially, copying each to the output buffer
      // Similar to fastCatOutDim0 operation in deep learning frameworks
      size_t totalProcessedBytes = 0;

      if (prefetch_dist_ > 0) {
        // With prefetching - improves performance by loading data into cache
        // before it's needed
        const int prefetchDist = prefetch_dist_;

        // Prefetch initial tensors
        for (int i = 1; i < prefetchDist && i < static_cast<int>(tensorsSize);
             i++) {
          __builtin_prefetch(allDataPtrs[i]);
        }

        // Process all tensors with prefetching
        for (size_t i = 0; i < tensorsSize; ++i) {
          const auto srcPtr = allDataPtrs[i];
          const auto inputNbytes = allNbytes[i];

          // Prefetch ahead
          if (i + prefetchDist < tensorsSize) {
            __builtin_prefetch(allDataPtrs[i + prefetchDist]);
          }

          if (inputNbytes == 0) {
            continue;
          }

          // Perform the memcpy operation
          std::memcpy(output + totalProcessedBytes, srcPtr, inputNbytes);
          totalProcessedBytes += inputNbytes;
        }
      } else {
        // Without prefetching - baseline implementation
        for (size_t i = 0; i < tensorsSize; ++i) {
          const auto srcPtr = allDataPtrs[i];
          const auto inputNbytes = allNbytes[i];

          if (inputNbytes == 0) {
            continue;
          }

          // Perform the memcpy operation
          std::memcpy(output + totalProcessedBytes, srcPtr, inputNbytes);
          totalProcessedBytes += inputNbytes;
        }
      }

      // Update statistics
      bytes_processed_[thread_id] += totalProcessedBytes;
      batches_processed_[thread_id]++;

      // Free the output tensor for this batch
      delete[] output;
    }
  }

  /**
   * Print benchmark results with multiple performance metrics:
   * 1. Bandwidth (GB/s) - Memory throughput
   * 2. Time per batch (microseconds) - Latency metric
   * 3. Operations per second - Throughput in batches/s
   */
  void printResults(double duration_seconds) {
    size_t total_bytes = 0;
    size_t total_batches = 0;

    for (size_t i = 0; i < num_threads_; ++i) {
      total_bytes += bytes_processed_[i];
      total_batches += batches_processed_[i];

      std::cout << "Thread " << i << ": Processed "
                << bytes_processed_[i] / (1024 * 1024) << " MB, "
                << batches_processed_[i] << " batches" << std::endl;
    }

    std::cout << "Total: Processed " << total_bytes / (1024 * 1024) << " MB, "
              << total_batches << " batches" << std::endl;
    std::cout << "Actual duration: " << duration_seconds << " seconds"
              << std::endl;

    // Calculate throughput in MB/s by dividing by the duration
    double throughput_gbps = static_cast<double>(total_bytes) /
        (1024 * 1024 * 1024) / duration_seconds;

    std::cout << "BW: " << throughput_gbps << " GB/s" << std::endl;

    double time_per_batch = duration_seconds * 1000000 / total_batches;
    std::cout << "Time/batch: " << time_per_batch << " us" << std::endl;

    // Calculate operations per second (each batch is an operation)
    double ops_per_second =
        static_cast<double>(total_batches) / duration_seconds;
    std::cout << "ops: " << ops_per_second << " batches/s" << std::endl;
  }

  // Member variables
  size_t num_threads_; // Number of worker threads
  std::vector<size_t> bytes_processed_; // Bytes processed by each thread
  std::vector<size_t> batches_processed_; // Batches processed by each thread
  std::atomic<bool> stop_flag_; // Flag to signal threads to stop
  size_t tensors_per_batch_; // Number of tensors per batch
  const SizeDistribution& size_dist_; // Reference to size distribution
  int prefetch_dist_; // Prefetch distance for optimization
};

/**
 * Main function - Parses command line arguments and runs the benchmark
 *
 * Command line options:
 * --threads            - Number of worker threads
 * --tensors            - Number of tensors per batch
 * --duration           - Benchmark duration in seconds
 * --size-dist          - Path to tensor size distribution file
 * --prefetch           - Prefetch distance (0 to disable)
 * --memory-pool-size   - Size of input tensor memory pool in GB
 * --output-tensor-size - Size of output tensor in bytes
 *
 * Supports both formats: --option value and --option=value
 */
int main(int argc, char* argv[]) {
  // Default parameters
  size_t num_threads = 4;
  size_t tensors_per_batch = 100;
  std::chrono::seconds duration(10);
  std::string size_dist_file;
  int prefetch_dist = DEFAULT_PREFETCH_DIST;
  size_t memory_pool_size_gb = 4; // Default memory pool size in GB

  // Parse command line arguments (supports both --option value and
  // --option=value formats)
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string option;
    std::string value;

    // Check if the argument is in the format --option=value
    size_t equals_pos = arg.find('=');
    if (equals_pos != std::string::npos) {
      option = arg.substr(0, equals_pos);
      value = arg.substr(equals_pos + 1);
    } else {
      option = arg;
      // For the traditional --option value format, value will be empty here
    }

    if (option == "--threads") {
      if (!value.empty()) {
        num_threads = std::stoul(value);
      } else if (i + 1 < argc) {
        num_threads = std::stoul(argv[++i]);
      }
    } else if (option == "--tensors") {
      if (!value.empty()) {
        tensors_per_batch = std::stoul(value);
      } else if (i + 1 < argc) {
        tensors_per_batch = std::stoul(argv[++i]);
      }
    } else if (option == "--duration") {
      if (!value.empty()) {
        duration = std::chrono::seconds(std::stoul(value));
      } else if (i + 1 < argc) {
        duration = std::chrono::seconds(std::stoul(argv[++i]));
      }
    } else if (option == "--size-dist") {
      if (!value.empty()) {
        size_dist_file = value;
      } else if (i + 1 < argc) {
        size_dist_file = argv[++i];
      }
    } else if (option == "--prefetch") {
      if (!value.empty()) {
        prefetch_dist = std::stoi(value);
      } else if (i + 1 < argc) {
        prefetch_dist = std::stoi(argv[++i]);
      }
    } else if (option == "--memory-pool-size") {
      if (!value.empty()) {
        memory_pool_size_gb = std::stoul(value);
      } else if (i + 1 < argc) {
        memory_pool_size_gb = std::stoul(argv[++i]);
      }
    } else if (option == "--output-tensor-size") {
      if (!value.empty()) {
        OUTPUT_TENSOR_SIZE = std::stoul(value);
      } else if (i + 1 < argc) {
        OUTPUT_TENSOR_SIZE = std::stoul(argv[++i]);
      }
    } else if (option == "--help") {
      std::cout
          << "Usage: " << argv[0] << " [options]" << std::endl
          << "Options:" << std::endl
          << "  --threads N       Number of threads (default: 4)" << std::endl
          << "  --tensors N       Tensors per batch (default: 100)" << std::endl
          << "  --duration N      Benchmark duration in seconds (default: 10)"
          << std::endl
          << "  --size-dist FILE  Size distribution file (default: random)"
          << std::endl
          << "  --prefetch N      Prefetch distance (default: 0, disabled)"
          << std::endl
          << "  --memory-pool-size N  Size of input memory pool in GB (default: 4)"
          << std::endl
          << "  --output-tensor-size N  Size of output tensor in bytes (default: 2421002)"
          << std::endl
          << "  --help            Show this help message" << std::endl;
      return 0;
    }
  }

  // Load tensor size distribution from file (required)
  SizeDistribution size_dist;
  if (!size_dist_file.empty()) {
    if (!size_dist.loadFromFile(size_dist_file)) {
      std::cerr << "Failed to load size distribution" << std::endl;
      exit(1);
    }
  } else {
    std::cout << "No size distribution file specified" << std::endl;
    exit(1);
  }

  // Initialize the large input memory pool for realistic memory access patterns
  std::cout << "Using memory pool size: " << memory_pool_size_gb << " GB"
            << std::endl;
  std::cout << "Using output tensor size: " << OUTPUT_TENSOR_SIZE << " bytes"
            << std::endl;
  g_input_memory_pool = std::make_unique<InputMemoryPool>(memory_pool_size_gb);

  // Create and run benchmark with online batch generation
  // Batches are generated on-the-fly during benchmarking for realism
  RebatchBenchmark benchmark(
      num_threads, tensors_per_batch, size_dist, prefetch_dist);
  benchmark.run(duration);

  // Clean up the input memory pool
  g_input_memory_pool.reset();

  return 0;
}
