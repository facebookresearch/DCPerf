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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/CppAttributes.h>
#include <folly/coro/Task.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>

namespace facebook::cea::chips::adsim {

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

  /**
   * Load size distribution from a CSV file
   * Format: size,frequency where frequency is a percentage (0-100)
   * Example: 24460,0.69
   */
  bool loadFromFile(const std::string& filename);

  /**
   * Sample a random size using binary search on the CDF
   * Time complexity: O(log n) where n is the number of unique sizes
   */
  size_t getRandomSize() const;
};

/**
 * InputMemoryPool - Large pre-allocated memory pool for input tensors
 *
 * Allocates a single large memory chunk to simulate realistic memory
 * environment. Tensors are positioned at random offsets within this pool.
 */
class InputMemoryPool {
 public:
  explicit InputMemoryPool(size_t size_gb);
  ~InputMemoryPool();

  // Delete copy constructor and copy assignment operator
  InputMemoryPool(const InputMemoryPool&) = delete;
  InputMemoryPool& operator=(const InputMemoryPool&) = delete;

  // Delete move constructor and move assignment operator
  InputMemoryPool(InputMemoryPool&&) = delete;
  InputMemoryPool& operator=(InputMemoryPool&&) = delete;

  void* FOLLY_NULLABLE getPointerAtOffset(size_t offset) const;
  size_t getSize() const;

 private:
  void* memory_ = nullptr;
  size_t size_ = 0;
};

/**
 * TensorBatch - Collection of tensors with random offsets in memory pool
 *
 * Creates a batch of tensors with:
 * 1. Sizes sampled from the provided distribution
 * 2. Random offsets within the large memory pool
 * 3. Total size limited by output tensor size
 */
class TensorBatch {
 public:
  TensorBatch(
      size_t count,
      const SizeDistribution& size_dist,
      size_t max_total_size,
      const InputMemoryPool& memory_pool);
  ~TensorBatch();

  // Delete copy constructor and copy assignment operator
  TensorBatch(const TensorBatch&) = delete;
  TensorBatch& operator=(const TensorBatch&) = delete;

  // Delete move constructor and move assignment operator
  TensorBatch(TensorBatch&&) = delete;
  TensorBatch& operator=(TensorBatch&&) = delete;

  const std::vector<const void*>& getTensors() const;
  size_t getTensorSize(size_t index) const;
  size_t getTensorCount() const;
  size_t getTotalSize() const;

 private:
  std::vector<void*> tensors;
  std::vector<size_t> tensor_sizes;
  size_t total_size = 0;
};

/**
 * Rebatch kernel - Multi-threaded memory copy benchmark
 *
 * Performs rebatch operations (concatenating multiple tensors into a single
 * output tensor) with configurable batch sizes, tensor size distributions, and
 * threading.
 */
class Rebatch : public Kernel {
 public:
  explicit Rebatch(
      int tensors_per_batch,
      int num_threads,
      size_t output_tensor_size,
      size_t memory_pool_size_gb,
      int prefetch_dist,
      std::string input_str,
      std::string distribution_file,
      size_t num_pregenerated_batches = 100);

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;

  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int64_t tensors_per_batch = config_d.count("tensors_per_batch")
        ? static_cast<int64_t>(config_d["tensors_per_batch"].asInt())
        : 100;
    int64_t num_threads = config_d.count("num_threads")
        ? static_cast<int64_t>(config_d["num_threads"].asInt())
        : 4;
    size_t output_tensor_size = config_d.count("output_tensor_size")
        ? static_cast<size_t>(config_d["output_tensor_size"].asInt())
        : 2421002;
    size_t memory_pool_size_gb = config_d.count("memory_pool_size_gb")
        ? static_cast<size_t>(config_d["memory_pool_size_gb"].asInt())
        : 4;
    int64_t prefetch_dist = config_d.count("prefetch_dist")
        ? static_cast<int64_t>(config_d["prefetch_dist"].asInt())
        : 0;
    std::string input_str = "Rebatch";
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    std::string distribution_file = config_d["distribution_file"].asString();

    size_t num_pregenerated_batches = config_d.count("num_pregenerated_batches")
        ? static_cast<size_t>(config_d["num_pregenerated_batches"].asInt())
        : 100;

    return std::make_shared<Rebatch>(
        tensors_per_batch,
        num_threads,
        output_tensor_size,
        memory_pool_size_gb,
        prefetch_dist,
        input_str,
        distribution_file,
        num_pregenerated_batches);
  }

 private:
  int64_t tensors_per_batch_;
  int64_t num_threads_;
  size_t output_tensor_size_;
  size_t memory_pool_size_gb_;
  int64_t prefetch_dist_;
  std::string input_str_;
  std::string distribution_file_;
  size_t num_pregenerated_batches_;

  std::shared_ptr<SizeDistribution> size_distribution_;
  std::shared_ptr<InputMemoryPool> input_memory_pool_;

  // Pregenerated batches for performance optimization
  std::vector<std::shared_ptr<TensorBatch>> pregenerated_batches_;
  std::atomic<size_t> batch_index_{0};

  // Pregenerate N batches to avoid on-the-fly generation
  void pregenerateBatches();

  // Get next pregenerated batch in round-robin fashion
  std::shared_ptr<TensorBatch> getNextBatch();

  // Single-threaded rebatch operation
  int rebatch_single_thread();

  // Multi-threaded rebatch operation
  folly::coro::Task<int> rebatch_multi_thread(
      std::shared_ptr<folly::Executor> pool);
};

} // namespace facebook::cea::chips::adsim
