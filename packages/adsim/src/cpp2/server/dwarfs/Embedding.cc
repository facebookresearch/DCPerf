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

#include <cea/chips/adsim/cpp2/server/dwarfs/Embedding.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>

#include <folly/coro/Collect.h>
#include <glog/logging.h>
#include "fbgemm/Fbgemm.h"

using fbgemm::GenerateEmbeddingSpMDMNBitWithStrides;
using fbgemm::GenerateEmbeddingSpMDMWithStrides;

namespace facebook::cea::chips::adsim {

DEFINE_timeseries(
    embedding_nfired,
    facebook::fb303::SUM,
    facebook::fb303::RATE);

// Single embedding table with quantized weights and FBGEMM kernel dispatch
EmbeddingTable::EmbeddingTable(const EmbeddingTableSpec& spec)
    : spec_(spec), generator_(42) {
  init_table();
}

EmbeddingTable::~EmbeddingTable() = default;

// Initialize table memory layout: quantized weights + scale/bias per row
void EmbeddingTable::init_table() {
  fused_embedding_dim_ = get_fused_embedding_dim();
  fused_embedding_table_.resize(spec_.num_embeddings * fused_embedding_dim_);
  fill_random_weights();
}

// Calculate fused row size: quantized weights + scale/bias floats
size_t EmbeddingTable::get_fused_embedding_dim() const {
  size_t base_dim = 0;

  // Calculate bytes needed for quantized weights
  switch (spec_.weights_precision) {
    case WeightsPrecision::INT4:
      base_dim = (spec_.embedding_dim + 1) / 2; // 4-bit packed
      break;
    case WeightsPrecision::INT8:
      base_dim = spec_.embedding_dim; // 1 byte per element
      break;
    case WeightsPrecision::FP16:
      base_dim = spec_.embedding_dim * 2; // 2 bytes per element
      break;
    case WeightsPrecision::FP32:
      base_dim = spec_.embedding_dim * 4; // 4 bytes per element
      break;
  }

  // Add space for scale and bias (2 floats = 8 bytes)
  return base_dim + 2 * sizeof(float);
}

// Initialize embedding table with random quantized weights and scale/bias
void EmbeddingTable::fill_random_weights() {
  const size_t weights_size = fused_embedding_dim_ - 2 * sizeof(float);

  // Parallel initialization for better performance
  const int num_threads =
      std::min(static_cast<int>(std::thread::hardware_concurrency()), 16);
  const int rows_per_thread =
      (spec_.num_embeddings + num_threads - 1) / num_threads;

  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    int start_row = t * rows_per_thread;
    int end_row = std::min(start_row + rows_per_thread, spec_.num_embeddings);

    if (start_row >= end_row) {
      break;
    }

    futures.emplace_back(
        std::async(
            std::launch::async, [this, start_row, end_row, weights_size]() {
              std::minstd_rand fast_gen(42 + start_row);
              std::uniform_int_distribution<uint8_t> byte_dist(0, 255);

              for (int i = start_row; i < end_row; ++i) {
                uint8_t* row_ptr =
                    fused_embedding_table_.data() + i * fused_embedding_dim_;

                // Fill quantized weights with random bytes
                for (size_t j = 0; j < weights_size; ++j) {
                  row_ptr[j] = byte_dist(fast_gen);
                }

                // Set scale and bias for quantized weights
                float* scale_bias =
                    reinterpret_cast<float*>(row_ptr + weights_size);
                scale_bias[0] = 2.0f; // scale
                scale_bias[1] = 1.0f; // bias
              }
            }));
  }

  for (auto& future : futures) {
    future.wait();
  }
}

void EmbeddingTable::forward(
    const EmbeddingRequest& request,
    void* output,
    PoolingMode pooling_mode,
    bool int32_indices) const {
  const float* weights =
      request.weighted ? request.per_sample_weights.data() : nullptr;
  const int lengths_sum = static_cast<int>(request.indices.size());
  const bool fp16 = OutputDtype::FP16 == spec_.output_dtype;
  if (int32_indices && fp16) {
    embedding_lookup_and_pool(
        request.indices32.data(),
        lengths_sum,
        request.offsets,
        weights,
        static_cast<fbgemm::float16*>(output),
        request.weighted,
        pooling_mode);
  } else if (int32_indices) {
    embedding_lookup_and_pool(
        request.indices32.data(),
        lengths_sum,
        request.offsets,
        weights,
        static_cast<float*>(output),
        request.weighted,
        pooling_mode);
  } else if (fp16) {
    embedding_lookup_and_pool(
        request.indices.data(),
        lengths_sum,
        request.offsets,
        weights,
        static_cast<fbgemm::float16*>(output),
        request.weighted,
        pooling_mode);
  } else {
    embedding_lookup_and_pool(
        request.indices.data(),
        lengths_sum,
        request.offsets,
        weights,
        static_cast<float*>(output),
        request.weighted,
        pooling_mode);
  }
}

// Perform embedding lookup and pooling using optimized FBGEMM kernels
template <typename IndexType, typename OutType>
void EmbeddingTable::embedding_lookup_and_pool(
    const IndexType* indices,
    int lengths_sum,
    const std::vector<int32_t>& offsets,
    const float* weights,
    OutType* output,
    bool weighted,
    PoolingMode pooling_mode) const {
  int batch_size = static_cast<int>(offsets.size()) - 1;

  bool success = false;
  const bool has_weight = weighted;
  const bool normalize =
      (pooling_mode == PoolingMode::MEAN); // MEAN pooling divides by bag length
  const int prefetch = 16; // FBGEMM prefetch distance
  const int output_stride = spec_.embedding_dim;
  const int input_stride = static_cast<int>(fused_embedding_dim_);
  const bool is_bf16_out = false;

  // Dispatch to appropriate FBGEMM kernel based on quantization precision
  switch (spec_.weights_precision) {
    case WeightsPrecision::FP32: {
      auto kernel = GenerateEmbeddingSpMDMWithStrides<
          float,
          IndexType,
          int32_t,
          OutType,
          true>(
          spec_.embedding_dim,
          has_weight,
          normalize,
          prefetch,
          false,
          true,
          output_stride,
          input_stride / sizeof(float),
          false,
          is_bf16_out);

      success = kernel(
          batch_size,
          lengths_sum,
          spec_.num_embeddings,
          reinterpret_cast<const float*>(fused_embedding_table_.data()),
          indices,
          offsets.data(),
          weights,
          output);
      break;
    }

    case WeightsPrecision::FP16: {
      auto kernel = GenerateEmbeddingSpMDMWithStrides<
          fbgemm::float16,
          IndexType,
          int32_t,
          OutType,
          true>(
          spec_.embedding_dim,
          has_weight,
          normalize,
          prefetch,
          false,
          true,
          output_stride,
          input_stride / sizeof(fbgemm::float16),
          false,
          is_bf16_out);

      success = kernel(
          batch_size,
          lengths_sum,
          spec_.num_embeddings,
          reinterpret_cast<const fbgemm::float16*>(
              fused_embedding_table_.data()),
          indices,
          offsets.data(),
          weights,
          output);
      break;
    }

    case WeightsPrecision::INT8: {
      auto kernel = GenerateEmbeddingSpMDMWithStrides<
          uint8_t,
          IndexType,
          int32_t,
          OutType,
          true>(
          spec_.embedding_dim,
          has_weight,
          normalize,
          prefetch,
          false,
          true,
          output_stride,
          input_stride,
          true,
          is_bf16_out);

      success = kernel(
          batch_size,
          lengths_sum,
          spec_.num_embeddings,
          fused_embedding_table_.data(),
          indices,
          offsets.data(),
          weights,
          output);
      break;
    }

    case WeightsPrecision::INT4: {
      const int bit_rate = 4;
      auto kernel = GenerateEmbeddingSpMDMNBitWithStrides<
          IndexType,
          int32_t,
          OutType,
          true>(
          bit_rate,
          spec_.embedding_dim,
          has_weight,
          normalize,
          prefetch,
          false,
          true,
          output_stride,
          input_stride,
          true);

      success = kernel(
          batch_size,
          lengths_sum,
          spec_.num_embeddings,
          fused_embedding_table_.data(),
          indices,
          offsets.data(),
          weights,
          output);
      break;
    }

    default: {
      LOG(ERROR) << "Unsupported weights precision: "
                 << static_cast<int>(spec_.weights_precision);
      success = false;
      break;
    }
  }

  if (!success) {
    LOG(ERROR) << "FBGEMM embedding operation failed for precision: "
               << static_cast<int>(spec_.weights_precision);
  }
}

size_t EmbeddingTable::get_memory_usage() const {
  return fused_embedding_table_.size();
}

// Multi-table embedding kernel with weighted selection and parallel
// initialization
Embedding::Embedding(
    std::vector<EmbeddingTableSpec> table_specs,
    int requests_per_fire,
    int nthreads,
    bool weighted,
    PoolingMode pooling_mode,
    int pregenerated_req_num,
    bool all_tables,
    bool int32_indices)
    : table_specs_(std::move(table_specs)),
      requests_per_fire_(
          all_tables ? static_cast<int>(table_specs_.size())
                     : requests_per_fire),
      nthreads_(nthreads),
      weighted_(weighted),
      pooling_mode_(pooling_mode),
      pregenerated_req_num_(pregenerated_req_num),
      all_tables_(all_tables),
      int32_indices_(int32_indices),
      generator_(42) {
  if (all_tables_) {
    // Whole rounds over the tables, so every fire starts at table 0
    const int n = static_cast<int>(table_specs_.size());
    pregenerated_req_num_ = std::max(n, pregenerated_req_num_ / n * n);
  }
  // Build cumulative distribution for O(log n) weighted table selection
  cumulative_weights_.resize(table_specs_.size());
  float total_weight = 0.0f;
  for (size_t i = 0; i < table_specs_.size(); ++i) {
    total_weight += table_specs_[i].access_weight;
    cumulative_weights_[i] = total_weight;
  }

  // Normalize to [0,1] for uniform random sampling
  if (total_weight > 0.0f) {
    for (auto& weight : cumulative_weights_) {
      weight /= total_weight;
    }
  }

  tables_.resize(table_specs_.size());

  auto start_time = std::chrono::high_resolution_clock::now();

  // Parallel table initialization to reduce startup latency
  const int num_threads = std::min(
      static_cast<int>(std::thread::hardware_concurrency()),
      static_cast<int>(table_specs_.size()));

  if (num_threads > 1 && table_specs_.size() > 1) {
    std::vector<std::future<void>> futures;
    futures.reserve(num_threads);

    const int tables_per_thread =
        static_cast<int>((table_specs_.size() + num_threads - 1) / num_threads);

    for (int t = 0; t < num_threads; ++t) {
      int start_idx = t * tables_per_thread;
      int end_idx = std::min(
          start_idx + tables_per_thread, static_cast<int>(table_specs_.size()));

      if (start_idx >= end_idx) {
        break;
      }

      futures.emplace_back(
          std::async(std::launch::async, [this, start_idx, end_idx]() {
            for (int i = start_idx; i < end_idx; ++i) {
              tables_[i] = std::make_unique<EmbeddingTable>(table_specs_[i]);
            }
          }));
    }

    for (auto& future : futures) {
      future.wait();
    }
  } else {
    for (size_t i = 0; i < table_specs_.size(); ++i) {
      tables_[i] = std::make_unique<EmbeddingTable>(table_specs_[i]);
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);

  LOG(INFO) << "Created " << table_specs_.size() << " embedding tables in "
            << duration.count() << "ms using " << num_threads << " threads";

  // Calculate maximum output buffer size needed
  max_output_size_ = 0;
  for (const auto& spec : table_specs_) {
    size_t output_size = spec.batch_size * spec.embedding_dim;
    max_output_size_ = std::max(max_output_size_, output_size);
  }

  // Allocate per-thread output buffers to avoid repeated allocations
  // Pre-generate table selections and requests for runtime efficiency
  pregenerate_pools();
}

// Pre-generate single pool of requests with table indices for simple
// distribution
void Embedding::pregenerate_pools() {
  pregenerated_requests_.reserve(pregenerated_req_num_);
  std::mt19937 gen(42);

  for (int i = 0; i < pregenerated_req_num_; ++i) {
    // Select table using weighted distribution
    auto rand_bits = gen();
    float random_value =
        static_cast<float>(rand_bits) / static_cast<float>(std::mt19937::max());

    auto it = std::lower_bound(
        cumulative_weights_.begin(), cumulative_weights_.end(), random_value);
    int table_idx = (it == cumulative_weights_.end())
        ? static_cast<int>(cumulative_weights_.size() - 1)
        : static_cast<int>(std::distance(cumulative_weights_.begin(), it));
    if (all_tables_) {
      table_idx = i % static_cast<int>(table_specs_.size());
    }

    const auto& spec = table_specs_[table_idx];

    // Pre-compute offsets for this table
    std::vector<int32_t> offsets(spec.batch_size + 1);
    offsets[0] = 0;
    for (int b = 0; b < spec.batch_size; ++b) {
      offsets[b + 1] = offsets[b] + spec.bag_size;
    }

    const int total_indices = spec.batch_size * spec.bag_size;
    std::vector<int64_t> indices(total_indices);
    std::vector<float> weights;

    // Fast index generation
    for (int j = 0; j < total_indices; ++j) {
      indices[j] = static_cast<int64_t>(gen() % spec.num_embeddings);
    }

    // Fast weight generation if needed
    if (weighted_ && total_indices > 0) {
      weights.resize(total_indices);
      // Explicit check to satisfy clang-tidy array bounds warning
      if (!weights.empty()) {
        for (int j = 0; j < total_indices; ++j) {
          auto weight_bits = gen();
          weights[j] = 0.9f +
              0.2f * (static_cast<float>(weight_bits & 0xFFFF) / 65535.0f);
        }
      }
    }

    // Create pre-generated request with table index
    EmbeddingRequest request(
        std::move(indices),
        std::move(offsets),
        weighted_ ? std::move(weights) : std::vector<float>{},
        weighted_);
    if (int32_indices_) {
      request.indices32.assign(request.indices.begin(), request.indices.end());
    }
    pregenerated_requests_.emplace_back(table_idx, std::move(request));
  }

  LOG(INFO) << "Pre-generated " << pregenerated_req_num_ << " requests across "
            << table_specs_.size() << " tables";
}

std::string Embedding::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs) {
  Kernel::init(std::move(h_objs), std::move(r_objs));

  // Log embedding table information
  size_t total_memory = 0;
  for (size_t i = 0; i < tables_.size(); ++i) {
    const auto& spec = table_specs_[i];
    size_t table_memory = tables_[i]->get_memory_usage();
    total_memory += table_memory;

    LOG(INFO) << "Embedding Table " << i << ": "
              << "rows=" << spec.num_embeddings
              << ", dim=" << spec.embedding_dim
              << ", bag_size=" << spec.bag_size
              << ", precision=" << static_cast<int>(spec.weights_precision)
              << ", memory=" << (table_memory / 1e6) << " MB";
  }

  LOG(INFO) << "Total embedding memory: " << (total_memory / 1e9) << " GB";

  return "";
}

// Execute benchmark fire operation with multi-threaded coroutine support
folly::coro::Task<std::string> Embedding::fire(
    std::shared_ptr<AdSimHandleObjs> /* h_objs */,
    std::shared_ptr<AdSimRequestObjs> /* r_objs */,
    std::shared_ptr<folly::Executor> pool) {
  STATS_embedding_nfired.add(1);

  const int nslices = std::max(nthreads_, 1);
  const size_t first = cursor_.fetch_add(
      static_cast<size_t>(requests_per_fire_) * nslices,
      std::memory_order_relaxed);
  if (0 == nthreads_) {
    run_embedding_operations(first, requests_per_fire_);
    co_return "";
  }
  std::vector<folly::Future<folly::Unit>> futures;
  futures.reserve(nthreads_);
  for (int i = 0; i < nthreads_; ++i) {
    futures.push_back(folly::via(pool.get(), [this, first, i]() {
      run_embedding_operations(
          first + static_cast<size_t>(i) * requests_per_fire_,
          requests_per_fire_);
    }));
  }
  co_await folly::collectAll(futures);
  co_return "";
}

void Embedding::run_embedding_operations(size_t first, int count) {
  // One output buffer per thread: concurrent fires do not share it
  thread_local std::vector<float> output;
  if (output.size() < max_output_size_) {
    output.resize(max_output_size_);
  }
  const size_t pool_size = pregenerated_requests_.size();
  for (int req = 0; req < count; ++req) {
    const auto& pregenerated_req =
        pregenerated_requests_[(first + req) % pool_size];
    tables_[pregenerated_req.table_idx]->forward(
        pregenerated_req.request, output.data(), pooling_mode_, int32_indices_);
  }
}

// Static parsing functions
WeightsPrecision Embedding::parse_weights_precision(const std::string& str) {
  if (str == "int4") {
    return WeightsPrecision::INT4;
  }
  if (str == "int8") {
    return WeightsPrecision::INT8;
  }
  if (str == "fp16") {
    return WeightsPrecision::FP16;
  }
  if (str == "fp32") {
    return WeightsPrecision::FP32;
  }
  return WeightsPrecision::INT4; // default
}

OutputDtype Embedding::parse_output_dtype(const std::string& str) {
  if (str == "fp16") {
    return OutputDtype::FP16;
  }
  if (str == "fp32") {
    return OutputDtype::FP32;
  }
  return OutputDtype::FP32; // default
}

PoolingMode Embedding::parse_pooling_mode(const std::string& str) {
  if (str == "sum") {
    return PoolingMode::SUM;
  }
  if (str == "mean") {
    return PoolingMode::MEAN;
  }
  if (str == "none") {
    return PoolingMode::NONE;
  }
  return PoolingMode::SUM; // default
}

std::shared_ptr<Kernel> Embedding::config(const folly::dynamic& config_d) {
  std::vector<EmbeddingTableSpec> table_specs;

  // Parse global configuration
  int requests_per_fire =
      static_cast<int>(config_d.getDefault("requests_per_fire", 1).asInt());
  int nthreads = static_cast<int>(config_d.getDefault("nthreads", 0).asInt());
  bool weighted = config_d.getDefault("weighted", false).asBool();
  int pregenerated_req_num = static_cast<int>(
      config_d.getDefault("pregenerated_req_num", 10000).asInt());
  bool all_tables = config_d.getDefault("all_tables", false).asBool();
  const auto index_dtype =
      config_d.getDefault("index_dtype", "int64").asString();
  if ("int32" != index_dtype && "int64" != index_dtype) {
    LOG(ERROR) << "Embedding index_dtype must be int32 or int64, not "
               << index_dtype;
    return nullptr;
  }

  auto weights_precision = parse_weights_precision(
      config_d.getDefault("weights_precision", "int4").asString());
  auto output_dtype = parse_output_dtype(
      config_d.getDefault("output_dtype", "fp32").asString());
  auto pooling_mode =
      parse_pooling_mode(config_d.getDefault("pooling", "sum").asString());

  // Parse comma-separated lists helper
  auto parse_int_list = [](const std::string& str) {
    std::vector<int> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
      result.push_back(std::stoi(item));
    }
    return result;
  };

  auto parse_float_list = [](const std::string& str) {
    std::vector<float> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
      result.push_back(std::stof(item));
    }
    return result;
  };

  // Helper function to get parameter values (single or list)
  auto get_int_values = [&](const std::string& single_key,
                            const std::string& list_key,
                            int default_val) {
    std::vector<int> values;
    if (config_d.count(list_key)) {
      values = parse_int_list(config_d[list_key].asString());
    } else if (config_d.count(single_key)) {
      values.push_back(static_cast<int>(config_d[single_key].asInt()));
    } else {
      values.push_back(default_val);
    }
    return values;
  };

  auto get_float_values = [&](const std::string& single_key,
                              const std::string& list_key,
                              float default_val) {
    std::vector<float> values;
    if (config_d.count(list_key)) {
      values = parse_float_list(config_d[list_key].asString());
    } else if (config_d.count(single_key)) {
      values.push_back(static_cast<float>(config_d[single_key].asDouble()));
    } else {
      values.push_back(default_val);
    }
    return values;
  };

  // Get parameter values
  auto num_embeddings_values =
      get_int_values("num_embeddings", "num_embeddings_list", 100000);
  auto embedding_dim_values =
      get_int_values("embedding_dim", "embedding_dim_list", 128);
  auto bag_size_values = get_int_values("bag_size", "bag_size_list", 20);
  auto batch_size_values = get_int_values("batch_size", "batch_size_list", 512);
  auto access_weight_values =
      get_float_values("access_weight", "access_weight_list", 1.0f);

  // Determine number of tables
  size_t max_size = std::max(
      {num_embeddings_values.size(),
       embedding_dim_values.size(),
       bag_size_values.size(),
       batch_size_values.size(),
       access_weight_values.size()});

  // Check for mismatched sizes
  std::vector<size_t> sizes = {
      num_embeddings_values.size(),
      embedding_dim_values.size(),
      bag_size_values.size(),
      batch_size_values.size(),
      access_weight_values.size()};

  bool has_mismatch = false;
  for (size_t size : sizes) {
    if (size != 1 && size != max_size) {
      has_mismatch = true;
      break;
    }
  }

  if (has_mismatch) {
    LOG(ERROR)
        << "Configuration error: Mismatched list sizes in embedding configuration.";
    LOG(ERROR)
        << "Help: All parameter lists must have the same size, or be single values.";
    LOG(ERROR) << "  num_embeddings: " << num_embeddings_values.size()
               << " values";
    LOG(ERROR) << "  embedding_dim: " << embedding_dim_values.size()
               << " values";
    LOG(ERROR) << "  bag_size: " << bag_size_values.size() << " values";
    LOG(ERROR) << "  batch_size: " << batch_size_values.size() << " values";
    LOG(ERROR) << "  access_weight: " << access_weight_values.size()
               << " values";
    LOG(ERROR) << "Expected: All lists should have " << max_size
               << " values or be single values.";
    return nullptr;
  }

  // Handle special case for num_tables parameter (legacy support)
  if (max_size == 1 && config_d.count("num_tables")) {
    int num_tables = static_cast<int>(config_d["num_tables"].asInt());
    max_size = static_cast<size_t>(num_tables);
  }

  // Expand single values to match max_size
  auto expand_to_size = [](std::vector<int>& vec, size_t target_size) {
    if (vec.size() == 1 && target_size > 1) {
      int val = vec[0];
      vec.resize(target_size, val);
    }
  };

  auto expand_float_to_size = [](std::vector<float>& vec, size_t target_size) {
    if (vec.size() == 1 && target_size > 1) {
      float val = vec[0];
      vec.resize(target_size, val);
    }
  };

  expand_to_size(num_embeddings_values, max_size);
  expand_to_size(embedding_dim_values, max_size);
  expand_to_size(bag_size_values, max_size);
  expand_to_size(batch_size_values, max_size);
  expand_float_to_size(access_weight_values, max_size);

  // Create table specs
  table_specs.reserve(max_size);
  for (size_t i = 0; i < max_size; ++i) {
    table_specs.emplace_back(
        num_embeddings_values[i],
        embedding_dim_values[i],
        bag_size_values[i],
        batch_size_values[i],
        access_weight_values[i],
        weights_precision,
        output_dtype);
  }

  return std::make_shared<Embedding>(
      std::move(table_specs),
      requests_per_fire,
      nthreads,
      weighted,
      pooling_mode,
      pregenerated_req_num,
      all_tables,
      "int32" == index_dtype);
}

} // namespace facebook::cea::chips::adsim
