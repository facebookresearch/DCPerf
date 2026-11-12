/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include <folly/coro/BlockingWait.h>
#include <folly/json/dynamic.h>
#include <gtest/gtest.h>

#include "fbgemm/FloatConversion.h"

using namespace facebook::cea::chips::adsim;

namespace {

constexpr int kRows = 64;
constexpr int kDim = 16;
constexpr int kBatch = 4;
constexpr int kBag = 3;

EmbeddingRequest makeRequest(bool weighted) {
  std::vector<int64_t> indices;
  std::vector<int32_t> offsets = {0};
  std::vector<float> weights;
  for (int b = 0; kBatch > b; ++b) {
    for (int k = 0; kBag > k; ++k) {
      indices.push_back((b * 17 + k * 5) % kRows);
      weights.push_back(0.5f + 0.25f * k);
    }
    offsets.push_back(offsets.back() + kBag);
  }
  EmbeddingRequest request(
      indices, offsets, weighted ? weights : std::vector<float>{}, weighted);
  request.indices32.assign(indices.begin(), indices.end());
  return request;
}

std::vector<float> lookup(
    WeightsPrecision precision,
    OutputDtype out,
    bool weighted,
    bool int32_indices) {
  const EmbeddingTable table(
      EmbeddingTableSpec(kRows, kDim, kBag, kBatch, 1.0f, precision, out));
  const auto request = makeRequest(weighted);
  std::vector<float> result(kBatch * kDim);
  if (OutputDtype::FP16 == out) {
    std::vector<fbgemm::float16> half(kBatch * kDim);
    table.forward(request, half.data(), PoolingMode::SUM, int32_indices);
    for (size_t i = 0; half.size() > i; ++i) {
      result[i] = fbgemm::cpu_half2float(half[i]);
    }
  } else {
    table.forward(request, result.data(), PoolingMode::SUM, int32_indices);
  }
  return result;
}

} // namespace

// fp16 output and int32 indices give the fp32, int64 lookup's sums
TEST(EmbeddingTest, OutputAndIndexTypesAgree) {
  for (auto precision : {WeightsPrecision::INT4, WeightsPrecision::INT8}) {
    for (bool weighted : {false, true}) {
      const auto ref = lookup(precision, OutputDtype::FP32, weighted, false);
      ASSERT_NE(ref, std::vector<float>(ref.size(), 0.0f));
      EXPECT_EQ(lookup(precision, OutputDtype::FP32, weighted, true), ref);
      for (bool int32_indices : {false, true}) {
        const auto half =
            lookup(precision, OutputDtype::FP16, weighted, int32_indices);
        for (size_t i = 0; ref.size() > i; ++i) {
          EXPECT_NEAR(half[i], ref[i], 2e-3 * std::abs(ref[i]) + 1e-3)
              << static_cast<int>(precision) << (weighted ? " weighted" : "")
              << (int32_indices ? " int32" : "") << " at " << i;
        }
      }
    }
  }
}

// With all_tables, each fire looks up every table once and the next fire
// moves on to new requests
TEST(EmbeddingTest, AllTablesFireCoversEveryTable) {
  const folly::dynamic config = folly::dynamic::object("all_tables", true)(
      "num_embeddings_list", "64,32,16")("embedding_dim", kDim)(
      "bag_size", kBag)("batch_size", kBatch)("pregenerated_req_num", 10)(
      "weights_precision", "int8")("output_dtype", "fp16")(
      "index_dtype", "int32");
  const auto kernel =
      std::dynamic_pointer_cast<Embedding>(Embedding::config(config));
  ASSERT_NE(kernel, nullptr);
  for (size_t i = 0; 9 > i; ++i) {
    EXPECT_EQ(kernel->request_table(i), static_cast<int>(i % 3)) << i;
  }
  EXPECT_THROW(kernel->request_table(9), std::out_of_range);
  for (size_t fire = 1; 4 > fire; ++fire) {
    folly::coro::blockingWait(kernel->fire(nullptr, nullptr, nullptr));
    EXPECT_EQ(kernel->next_request(), 3 * fire);
  }
}

TEST(EmbeddingTest, RejectsUnknownIndexType) {
  const folly::dynamic config =
      folly::dynamic::object("num_embeddings", 64)("index_dtype", "int16");
  EXPECT_EQ(Embedding::config(config), nullptr);
}
