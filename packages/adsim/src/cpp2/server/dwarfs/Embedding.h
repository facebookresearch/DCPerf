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

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/Task.h>
#include <folly/json/dynamic.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(embedding_nfired);

// Quantization precision for embedding weights
enum class WeightsPrecision { INT4 = 4, INT8 = 8, FP16 = 16, FP32 = 32 };
// Output data type for embedding lookups
enum class OutputDtype { FP16 = 16, FP32 = 32 };
// Pooling aggregation mode for embedding bags
enum class PoolingMode { SUM, MEAN, NONE };

// Configuration for a single embedding table
struct EmbeddingTableSpec {
  int num_embeddings;
  int embedding_dim;
  int bag_size;
  int batch_size;
  float access_weight; // Probability weight for table access
  WeightsPrecision weights_precision;
  OutputDtype output_dtype;

  EmbeddingTableSpec(
      int ne,
      int ed,
      int bs,
      int batch_sz = 512,
      float weight = 1.0f,
      WeightsPrecision wp = WeightsPrecision::INT4,
      OutputDtype od = OutputDtype::FP32)
      : num_embeddings(ne),
        embedding_dim(ed),
        bag_size(bs),
        batch_size(batch_sz),
        access_weight(weight),
        weights_precision(wp),
        output_dtype(od) {}
};

// Embedding lookup request with sparse indices and optional weights
struct EmbeddingRequest {
  std::vector<int64_t> indices;
  std::vector<int32_t> indices32; // The same indices, when tables use int32
  std::vector<int32_t> offsets;
  std::vector<float> per_sample_weights;
  bool weighted;

  EmbeddingRequest(
      std::vector<int64_t> idx,
      std::vector<int32_t> off,
      std::vector<float> weights = {},
      bool w = false)
      : indices(std::move(idx)),
        offsets(std::move(off)),
        per_sample_weights(std::move(weights)),
        weighted(w) {}
};

// Single embedding table with quantized weights and FBGEMM kernels
class EmbeddingTable {
 public:
  explicit EmbeddingTable(const EmbeddingTableSpec& spec);
  ~EmbeddingTable();

  void fill_random_weights();
  // Writes batch_size x embedding_dim floats, or fp16 values for FP16 output.
  void forward(
      const EmbeddingRequest& request,
      void* output,
      PoolingMode pooling_mode = PoolingMode::SUM,
      bool int32_indices = false) const;

  size_t get_memory_usage() const;

 private:
  EmbeddingTableSpec spec_;
  std::vector<uint8_t> fused_embedding_table_; // Quantized weights + scale/bias
  size_t fused_embedding_dim_{};
  std::mt19937 generator_;

  void init_table();
  size_t get_fused_embedding_dim() const;
  template <typename IndexType, typename OutType>
  void embedding_lookup_and_pool(
      const IndexType* indices,
      int lengths_sum,
      const std::vector<int32_t>& offsets,
      const float* weights,
      OutType* output,
      bool weighted,
      PoolingMode pooling_mode) const;
};

// Multi-table embedding kernel for AdSim simulation
class Embedding : public Kernel {
 public:
  explicit Embedding(
      std::vector<EmbeddingTableSpec> table_specs,
      int requests_per_fire,
      int nthreads,
      bool weighted = false,
      PoolingMode pooling_mode = PoolingMode::SUM,
      int pregenerated_req_num = 10000,
      bool all_tables = false,
      bool int32_indices = false);

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;

  // "all_tables": each fire looks up every table once, in order, instead of
  // "requests_per_fire" tables drawn by access weight. "index_dtype": "int64"
  // (default) or "int32" indices. "output_dtype": "fp32" (default) or "fp16".
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d);

  // Index of the pre-generated request the next fire starts at.
  size_t next_request() const {
    return cursor_.load(std::memory_order_relaxed);
  }
  // Table that pre-generated request i looks up.
  int request_table(size_t i) const {
    return pregenerated_requests_.at(i).table_idx;
  }

 private:
  std::vector<EmbeddingTableSpec> table_specs_;
  std::vector<std::unique_ptr<EmbeddingTable>> tables_;
  std::vector<float>
      cumulative_weights_; // Cumulative weights for weighted table selection
  int requests_per_fire_; // Number of requests to execute per fire operation
  int nthreads_;
  bool weighted_;
  PoolingMode pooling_mode_;
  int pregenerated_req_num_; // Total number of pre-generated requests
  // Every fire looks up each table once, as one TBE forward does.
  bool all_tables_;
  bool int32_indices_;
  // Next pre-generated request, so successive fires touch different rows.
  std::atomic<size_t> cursor_{0};

  // Simplified pre-generated request pool
  struct PreGeneratedRequest {
    int table_idx;
    EmbeddingRequest request;

    PreGeneratedRequest(int idx, EmbeddingRequest req)
        : table_idx(idx), request(std::move(req)) {}
  };

  std::vector<PreGeneratedRequest>
      pregenerated_requests_; // Single pool of all requests

  size_t max_output_size_; // Floats in the largest table's output

  std::mt19937 generator_;

  void run_embedding_operations(size_t first, int count);

  // Pre-generation methods for runtime efficiency
  void pregenerate_pools();

  static WeightsPrecision parse_weights_precision(const std::string& str);
  static OutputDtype parse_output_dtype(const std::string& str);
  static PoolingMode parse_pooling_mode(const std::string& str);
};

} // namespace facebook::cea::chips::adsim
