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

#include <cea/chips/adsim/cpp2/server/dwarfs/BatchStageImpl.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::batch_stage {
namespace {

std::vector<int64_t> repetitionsFor(
    const Repeat& r,
    const std::vector<int64_t>& sizes) {
  std::vector<int64_t> out;
  out.reserve(sizes.size());
  for (const int64_t n : sizes) {
    out.push_back(repetitions(r, n));
  }
  return out;
}

TEST(BatchStageImplTest, BatchClosesAWindowAfterItOpens) {
  EXPECT_DOUBLE_EQ(closeDelayUs(22000, false, 25000), 22000);
}

TEST(BatchStageImplTest, PeriodicBatchClosesAtTheNextMultipleOfTheWindow) {
  EXPECT_DOUBLE_EQ(closeDelayUs(22000, true, 25000), 19000);
  EXPECT_DOUBLE_EQ(closeDelayUs(22000, true, 43999), 1);
}

TEST(BatchStageImplTest, DefaultRepeatRunsOncePerBatch) {
  const std::vector<int64_t> expected{1, 1, 1};
  EXPECT_EQ(repetitionsFor(Repeat{}, {1, 8, 16}), expected);
}

TEST(BatchStageImplTest, PerExampleRepeatRunsOncePerExample) {
  const std::vector<int64_t> expected{1, 8, 16};
  EXPECT_EQ(
      repetitionsFor(
          Repeat{.base = 0, .per_example = 1, .exponent = 1}, {1, 8, 16}),
      expected);
}

TEST(BatchStageImplTest, RepeatGrowsWithSizeToTheExponent) {
  // 2 + 0.5 * 4^2 = 10
  EXPECT_EQ(
      repetitions(Repeat{.base = 2, .per_example = 0.5, .exponent = 2}, 4), 10);
}

TEST(BatchStageImplTest, RejectsNegativeParameters) {
  EXPECT_THROW(validate(Repeat{.base = -1}), std::invalid_argument);
  EXPECT_THROW(validate(Repeat{.per_example = -1}), std::invalid_argument);
  EXPECT_THROW(validate(Repeat{.exponent = -1}), std::invalid_argument);
}

} // namespace
} // namespace facebook::cea::chips::adsim::batch_stage
