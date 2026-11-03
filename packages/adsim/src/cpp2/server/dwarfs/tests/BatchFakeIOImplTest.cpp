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

#include <cea/chips/adsim/cpp2/server/dwarfs/BatchFakeIOImpl.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::batch_fake_io {
namespace {

struct TestBatch {
  int64_t size = 0;
};

constexpr Params kParams{
    .window_us = 22000,
    .base_us = 3000,
    .per_request_us = 200,
    .exponent = 1.75,
    .sigma = 0.35,
};

TEST(BatchFakeIOImplTest, BatchTimeGrowsWithSizeToTheExponent) {
  // 16^1.75 = 2^7 = 128
  EXPECT_DOUBLE_EQ(batchTimeUs(kParams, 16), 3000 + 200 * 128);
  EXPECT_DOUBLE_EQ(batchTimeUs(kParams, 1), 3000 + 200);
}

TEST(BatchFakeIOImplTest, RequestsJoinTheOpenBatchUntilItCloses) {
  Batcher<TestBatch> batcher;
  const auto [first, first_opened] = batcher.join();
  const auto [second, second_opened] = batcher.join();
  const auto [third, third_opened] = batcher.join();

  const std::vector<bool> expected_opened{true, false, false};
  EXPECT_EQ(
      (std::vector<bool>{first_opened, second_opened, third_opened}),
      expected_opened);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, third);
  EXPECT_EQ(batcher.close(first), 3);
}

TEST(BatchFakeIOImplTest, RequestAfterCloseOpensANewBatch) {
  Batcher<TestBatch> batcher;
  const auto [old_batch, old_opened] = batcher.join();
  batcher.close(old_batch);

  const auto [new_batch, new_opened] = batcher.join();
  EXPECT_TRUE(new_opened);
  EXPECT_NE(new_batch, old_batch);
  EXPECT_EQ(batcher.close(new_batch), 1);
}

TEST(BatchFakeIOImplTest, NoiseHasMedianOne) {
  Noise noise(kParams.sigma, 42);
  std::vector<double> samples(10001);
  std::generate(samples.begin(), samples.end(), [&] { return noise.sample(); });
  std::nth_element(samples.begin(), samples.begin() + 5000, samples.end());
  EXPECT_NEAR(samples[5000], 1.0, 0.03);
}

TEST(BatchFakeIOImplTest, ZeroSigmaGivesNoNoise) {
  Noise noise(0.0, 42);
  EXPECT_EQ(noise.sample(), 1.0);
}

TEST(BatchFakeIOImplTest, NegativeParameterIsRejected) {
  Params params = kParams;
  params.per_request_us = -1;
  EXPECT_THROW(validate(params), std::invalid_argument);
}

} // namespace
} // namespace facebook::cea::chips::adsim::batch_fake_io
