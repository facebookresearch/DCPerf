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

#include <cea/chips/adsim/cpp2/server/dwarfs/Rebatch.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/futures/Future.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(rebatch_nfired);

/**
 * Load size distribution from a CSV file
 * Format: size,frequency where frequency is a percentage (0-100)
 * Example: 24460,0.69
 *
 * Builds a cumulative distribution function (CDF) for efficient sampling
 */
bool SizeDistribution::loadFromFile(const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    LOG(ERROR) << "Failed to open size distribution file: " << filename;
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
        sizeFreqPairs.emplace_back(size, freq / 100.0);
      } catch (const std::exception& e) {
        LOG(ERROR) << "Error parsing line: " << line << " - " << e.what();
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

  LOG(INFO) << "Loaded " << sizes.size()
            << " unique sizes with cumulative probability distribution";
  return true;
}

/**
 * Sample a random size using binary search on the CDF
 * Time complexity: O(log n) where n is the number of unique sizes
 * Uses thread-local RNG for better performance in multi-threaded scenarios
 */
size_t SizeDistribution::getRandomSize() const {
  if (sizes.empty()) {
    return 0;
  }

  // Initialize thread-local random generator
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

/**
 * InputMemoryPool constructor - Initialize with a large memory chunk (size in
 * GB)
 */
InputMemoryPool::InputMemoryPool(size_t size_gb) {
  // Convert GB to bytes
  size_t size_bytes = size_gb * 1024ULL * 1024ULL * 1024ULL;

  LOG(INFO) << "Allocating a large memory pool of " << size_gb << " GB ("
            << size_bytes << " bytes) for input tensors";

  // Allocate the memory chunk
  memory_ = malloc(size_bytes);
  if (!memory_) {
    throw std::bad_alloc();
  }

  // Initialize with some pattern for validation if needed
  memset(memory_, 0xAB, size_bytes);

  size_ = size_bytes;
}

InputMemoryPool::~InputMemoryPool() {
  if (memory_) {
    free(memory_);
    memory_ = nullptr;
  }
}

void* FOLLY_NULLABLE InputMemoryPool::getPointerAtOffset(size_t offset) const {
  if (offset < size_) {
    return static_cast<char*>(memory_) + offset;
  }
  return nullptr;
}

size_t InputMemoryPool::getSize() const {
  return size_;
}

/**
 * TensorBatch constructor - Collection of tensors with random offsets in memory
 * pool
 *
 * Creates a batch of tensors with:
 * 1. Sizes sampled from the provided distribution
 * 2. Random offsets within the large memory pool
 * 3. Total size limited by max_total_size
 */
TensorBatch::TensorBatch(
    size_t count,
    const SizeDistribution& size_dist,
    size_t max_total_size,
    const InputMemoryPool& memory_pool) {
  // Initialize random number generator for offsets
  static thread_local std::mt19937 rng(std::random_device{}());

  // Get the total size of the memory pool
  size_t pool_size = memory_pool.getSize();

  // Allocate memory for each tensor based on the size distribution
  for (size_t i = 0; i < count; ++i) {
    size_t size = size_dist.getRandomSize();
    if (size == 0) {
      continue;
    }

    // Check if adding this tensor would exceed the max total size
    if (total_size + size > max_total_size) {
      break;
    }

    // Calculate maximum possible offset to ensure we don't exceed the pool size
    if (size > pool_size) {
      continue; // Skip tensors that are larger than the entire pool
    }
    size_t max_offset = pool_size - size;

    // Generate a random offset within the valid range
    std::uniform_int_distribution<size_t> dist(0, max_offset);
    size_t offset = dist(rng);

    // Get a pointer at the random offset in the memory pool
    void* tensor = memory_pool.getPointerAtOffset(offset);

    if (tensor) {
      tensors.push_back(tensor);
      tensor_sizes.push_back(size);
      total_size += size;
    }
  }
}

TensorBatch::~TensorBatch() {
  // No need to free tensors - they're pre-allocated and managed by the memory
  // pool
  tensors.clear();
  tensor_sizes.clear();
}

const std::vector<const void*>& TensorBatch::getTensors() const {
  return reinterpret_cast<const std::vector<const void*>&>(tensors);
}

size_t TensorBatch::getTensorSize(size_t index) const {
  if (index < tensor_sizes.size()) {
    return tensor_sizes[index];
  }
  return 0;
}

size_t TensorBatch::getTensorCount() const {
  return tensors.size();
}

size_t TensorBatch::getTotalSize() const {
  return total_size;
}

/**
 * Rebatch kernel constructor
 *
 * Initializes benchmark with tensor size distribution and memory pool
 * configuration.
 */
Rebatch::Rebatch(
    int tensors_per_batch,
    int num_threads,
    size_t output_tensor_size,
    size_t memory_pool_size_gb,
    int prefetch_dist,
    std::string input_str,
    std::string distribution_file,
    size_t num_pregenerated_batches)
    : tensors_per_batch_(tensors_per_batch),
      num_threads_(num_threads),
      output_tensor_size_(output_tensor_size),
      memory_pool_size_gb_(memory_pool_size_gb),
      prefetch_dist_(prefetch_dist),
      input_str_(std::move(input_str)),
      distribution_file_(std::move(distribution_file)),
      num_pregenerated_batches_(num_pregenerated_batches) {
  LOG(INFO) << "Rebatch initialized with params: " << "tensors_per_batch="
            << tensors_per_batch_ << ", num_threads=" << num_threads_
            << ", output_tensor_size=" << output_tensor_size_
            << ", memory_pool_size_gb=" << memory_pool_size_gb_
            << ", prefetch_dist=" << prefetch_dist_
            << ", distribution_file=" << distribution_file_;
}

/**
 * Initialize rebatch kernel with distribution-based workload
 *
 * Loads tensor size distribution from CSV file and initializes memory pool.
 */
std::string Rebatch::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> /*unused*/) {
  // Load tensor size distribution from file
  size_distribution_ = std::make_shared<SizeDistribution>();
  if (!size_distribution_->loadFromFile(distribution_file_)) {
    throw std::runtime_error(
        "Failed to load size distribution file: " + distribution_file_);
  }

  // Initialize the large input memory pool for realistic memory access patterns
  input_memory_pool_ = std::make_shared<InputMemoryPool>(memory_pool_size_gb_);

  // Store references in handle objects for potential reuse
  h_objs->set_shared_ptr(input_str_ + "_size_dist", size_distribution_);
  h_objs->set_shared_ptr(input_str_ + "_memory_pool", input_memory_pool_);

  // Pregenerate batches for performance optimization
  pregenerateBatches();

  std::string info = folly::to<std::string>(
      "Rebatch: ", input_str_, " ", std::to_string(output_tensor_size_), "B");
  info += " (distribution: " + distribution_file_ + ")";
  info += " (memory_pool: " + std::to_string(memory_pool_size_gb_) + "GB)";
  info +=
      " (pregenerated_batches: " + std::to_string(num_pregenerated_batches_) +
      ")";
  return info;
}

/**
 * Pregenerate N batches to avoid on-the-fly generation during benchmark
 * execution
 *
 * Creates pregenerated_batches_ vector with N TensorBatch instances that can be
 * reused in a round-robin fashion during rebatch operations.
 */
void Rebatch::pregenerateBatches() {
  LOG(INFO) << "Pregenerating " << num_pregenerated_batches_
            << " batches for performance optimization";

  pregenerated_batches_.reserve(num_pregenerated_batches_);

  for (size_t i = 0; i < num_pregenerated_batches_; ++i) {
    auto batch = std::make_shared<TensorBatch>(
        tensors_per_batch_,
        *size_distribution_,
        output_tensor_size_,
        *input_memory_pool_);
    pregenerated_batches_.push_back(std::move(batch));
  }

  LOG(INFO) << "Successfully pregenerated " << pregenerated_batches_.size()
            << " batches";
}

/**
 * Get next pregenerated batch in round-robin fashion
 *
 * Returns a pregenerated batch from the pool, cycling through them in a
 * thread-safe manner using atomic operations.
 */
std::shared_ptr<TensorBatch> Rebatch::getNextBatch() {
  if (pregenerated_batches_.empty()) {
    throw std::runtime_error("No pregenerated batches available");
  }

  size_t index = batch_index_.fetch_add(1) % pregenerated_batches_.size();
  return pregenerated_batches_[index];
}

/**
 * Single-threaded rebatch operation
 *
 * Uses a pregenerated batch of tensors and performs sequential memory copy
 * operations.
 */
int Rebatch::rebatch_single_thread() {
  // Get a pregenerated batch for this operation (avoids on-the-fly generation)
  auto batch = getNextBatch();

  const auto& tensors = batch->getTensors();
  const size_t numTensors = tensors.size();

  if (numTensors == 0) {
    return 0; // Skip empty batches
  }

  // Allocate a fresh output tensor for this batch
  std::unique_ptr<char[]> output(new char[output_tensor_size_]);

  // Process all tensors sequentially, copying each to the output buffer
  size_t totalProcessedBytes = 0;
  uint64_t checksum = 0; // Prevent compiler optimization

  if (prefetch_dist_ > 0) {
    // With prefetching - improves performance by loading data into cache
    const int prefetchDist = static_cast<int>(prefetch_dist_);

    // Prefetch initial tensors
    for (int i = 1; i < prefetchDist && i < static_cast<int>(numTensors); i++) {
      __builtin_prefetch(tensors[i]);
    }

    // Process all tensors with prefetching
    for (size_t i = 0; i < numTensors; ++i) {
      const auto srcPtr = tensors[i];
      const auto inputNbytes = batch->getTensorSize(i);

      // Prefetch ahead
      if (i + prefetchDist < numTensors) {
        __builtin_prefetch(tensors[i + prefetchDist]);
      }

      if (inputNbytes == 0) {
        continue;
      }

      // Perform the memcpy operation
      memcpy(output.get() + totalProcessedBytes, srcPtr, inputNbytes);

      // Use the copied data to prevent compiler optimization
      // Calculate checksum from both source and destination data
      checksum ^= reinterpret_cast<uintptr_t>(srcPtr);
      if (inputNbytes >= sizeof(uint64_t)) {
        // XOR with first 8 bytes of copied data to ensure it's actually used
        checksum ^= *reinterpret_cast<const uint64_t*>(
            output.get() + totalProcessedBytes);
      }

      totalProcessedBytes += inputNbytes;
    }
  } else {
    // Without prefetching - baseline implementation
    for (size_t i = 0; i < numTensors; ++i) {
      const auto srcPtr = tensors[i];
      const auto inputNbytes = batch->getTensorSize(i);

      if (inputNbytes == 0) {
        continue;
      }

      // Perform the memcpy operation
      memcpy(output.get() + totalProcessedBytes, srcPtr, inputNbytes);

      // Use the copied data to prevent compiler optimization
      // Calculate checksum from both source and destination data
      checksum ^= reinterpret_cast<uintptr_t>(srcPtr);
      if (inputNbytes >= sizeof(uint64_t)) {
        // XOR with first 8 bytes of copied data to ensure it's actually used
        checksum ^= *reinterpret_cast<const uint64_t*>(
            output.get() + totalProcessedBytes);
      }

      totalProcessedBytes += inputNbytes;
    }
  }

  return static_cast<int>(totalProcessedBytes ^ checksum);
}

/**
 * Multi-threaded rebatch operation with work distribution
 *
 * Distributes rebatch work across worker threads. Each thread processes
 * a portion of the tensors in the batch.
 */
folly::coro::Task<int> Rebatch::rebatch_multi_thread(
    std::shared_ptr<folly::Executor> pool) {
  // Get a pregenerated batch for this operation (avoids on-the-fly generation)
  auto batch = getNextBatch();

  const auto& tensors = batch->getTensors();
  const size_t numTensors = tensors.size();

  if (numTensors == 0) {
    co_return 0; // Skip empty batches
  }

  // Allocate a fresh output tensor for this batch
  auto output = std::make_shared<std::vector<char>>(output_tensor_size_);

  std::vector<folly::Future<int>> futures;
  size_t tensors_per_thread = numTensors / num_threads_;

  // Create worker tasks with balanced load distribution
  for (int t = 0; t < num_threads_; ++t) {
    size_t thread_start = t * tensors_per_thread;
    size_t thread_end = (t == num_threads_ - 1)
        ? numTensors // Last thread handles remainder
        : (t + 1) * tensors_per_thread;

    auto future =
        folly::via(pool.get(), [batch, output, thread_start, thread_end]() {
          const auto& batch_tensors = batch->getTensors();
          size_t totalProcessedBytes = 0;
          uint64_t checksum = 0;

          // Calculate starting offset for this thread's portion
          size_t output_offset = 0;
          for (size_t i = 0; i < thread_start; ++i) {
            output_offset += batch->getTensorSize(i);
          }

          // Process assigned chunk of tensors
          for (size_t i = thread_start; i < thread_end; ++i) {
            const auto srcPtr = batch_tensors[i];
            const auto inputNbytes = batch->getTensorSize(i);

            if (inputNbytes == 0) {
              continue;
            }

            // Perform the memcpy operation
            memcpy(
                output->data() + output_offset + totalProcessedBytes,
                srcPtr,
                inputNbytes);

            // Use the copied data to prevent compiler optimization
            // Calculate checksum from both source and destination data
            checksum ^= reinterpret_cast<uintptr_t>(srcPtr);
            if (inputNbytes >= sizeof(uint64_t)) {
              // XOR with first 8 bytes of copied data to ensure it's actually
              // used
              checksum ^= *reinterpret_cast<const uint64_t*>(
                  output->data() + output_offset + totalProcessedBytes);
            }

            totalProcessedBytes += inputNbytes;
          }

          return static_cast<int>(totalProcessedBytes ^ checksum);
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

/**
 * Execute benchmark fire operation
 *
 * Performs single benchmark iteration with rebatch operations.
 * Selects single/multi-threaded execution based on configuration.
 */
folly::coro::Task<std::string> Rebatch::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool) {
  STATS_rebatch_nfired.add(1);

  // Select execution mode based on thread configuration and pool availability
  if (num_threads_ > 1 && pool) {
    co_await rebatch_multi_thread(std::move(pool));
  } else {
    rebatch_single_thread();
  }

  co_return "";
}

} // namespace facebook::cea::chips::adsim
