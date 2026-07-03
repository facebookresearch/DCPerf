// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Sanity tests for ranking::PercentileSampler. The fanout calibration in
// LeafNodeRank depends on these samplers reading rpc_dist.json correctly
// and producing values within the configured percentile range.
//
// Tests cover: load success, load failure modes, sample range bounds,
// linear interpolation correctness, and the legacy prefixed-keys file
// format used by DriverNodeRank's --request_size_distribution.

#include "PercentileSampler.h"

#include <random>

#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/portability/GTest.h>

namespace ranking {
namespace {

constexpr const char* kSampleJson = R"({
  "min": 100,
  "p05": 110,
  "p10": 120,
  "p50": 200,
  "p95": 900,
  "max": 1000
})";

TEST(PercentileSamplerTest, LoadFromDynamic_ValidObject) {
  auto obj = folly::parseJson(kSampleJson);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  EXPECT_TRUE(s.isLoaded());
  // Six recognized keys: min, p05, p10, p50, p95, max.
  EXPECT_EQ(s.numPoints(), 6u);
  EXPECT_EQ(s.minValue(), 100);
  EXPECT_EQ(s.maxValue(), 1000);
}

TEST(PercentileSamplerTest, LoadFromDynamic_RejectsNonObject) {
  folly::dynamic arr = folly::dynamic::array(1, 2, 3);
  PercentileSampler s;
  EXPECT_FALSE(s.loadFromDynamic(arr));
  EXPECT_FALSE(s.isLoaded());
}

TEST(PercentileSamplerTest, LoadFromDynamic_RejectsTooFewPoints) {
  // Only one recognized key — needs at least 2 to interpolate.
  folly::dynamic obj = folly::dynamic::object("min", 100);
  PercentileSampler s;
  EXPECT_FALSE(s.loadFromDynamic(obj));
  EXPECT_FALSE(s.isLoaded());
}

TEST(PercentileSamplerTest, Sample_AllValuesInRange) {
  auto obj = folly::parseJson(kSampleJson);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  std::mt19937 rng(42);
  for (int i = 0; i < 1000; ++i) {
    size_t v = s.sample(rng);
    EXPECT_GE(v, 100u);
    EXPECT_LE(v, 1000u);
  }
}

TEST(PercentileSamplerTest, SampleI64_AllValuesInRange) {
  auto obj = folly::parseJson(kSampleJson);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  std::mt19937 rng(99);
  for (int i = 0; i < 1000; ++i) {
    int64_t v = s.sampleI64(rng);
    EXPECT_GE(v, 100);
    EXPECT_LE(v, 1000);
  }
}

TEST(PercentileSamplerTest, ValueAtKnownPercentiles) {
  auto obj = folly::parseJson(kSampleJson);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  // Exact match at percentile boundaries.
  EXPECT_EQ(s.valueAtProbability(0.05), 110);
  EXPECT_EQ(s.valueAtProbability(0.10), 120);
  EXPECT_EQ(s.valueAtProbability(0.50), 200);
  EXPECT_EQ(s.valueAtProbability(0.95), 900);
  // Below min and above max clamp to boundaries.
  EXPECT_EQ(s.valueAtProbability(-0.5), 100);
  EXPECT_EQ(s.valueAtProbability(2.0), 1000);
}

TEST(PercentileSamplerTest, ValueAtInterpolatedPercentile) {
  // Linear interpolation between p05=110 and p10=120 at p=0.075:
  //   (0.075 - 0.05) / (0.10 - 0.05) = 0.5
  //   110 + 0.5 * (120 - 110) = 115
  auto obj = folly::parseJson(kSampleJson);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  EXPECT_EQ(s.valueAtProbability(0.075), 115);
}

TEST(PercentileSamplerTest, IgnoresMissingKeys) {
  // rpc_dist.json has no p99, so PercentileSampler must skip missing
  // keys without erroring.
  folly::dynamic obj = folly::dynamic::object("min", 100)("p50", 200)(
      "max", 1000);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  EXPECT_EQ(s.numPoints(), 3u);
  EXPECT_EQ(s.valueAtProbability(0.5), 200);
}

TEST(PercentileSamplerTest, IgnoresNegativeAndNonIntValues) {
  folly::dynamic obj = folly::dynamic::object("min", 100)("p05", -5)(
      "p10", "not a number")("p50", 200)("max", 1000);
  PercentileSampler s;
  ASSERT_TRUE(s.loadFromDynamic(obj));
  // min, p50, max -> 3 points loaded; p05 and p10 are dropped.
  EXPECT_EQ(s.numPoints(), 3u);
}

TEST(PercentileSamplerTest, LegacyPrefixedShape) {
  // The original RequestSizeSampler shape: array of one object with
  // prefixed keys. Used by DriverNodeRank's --request_size_distribution.
  // This test ensures the alias `using RequestSizeSampler = PercentileSampler`
  // continues to work for that callsite.
  folly::dynamic obj = folly::dynamic::object("req_size_min", 100)(
      "req_size_p50", 200)("req_size_max", 1000);
  folly::dynamic arr = folly::dynamic::array(obj);
  // Write to a temp file and round-trip through load(path, prefix).
  std::string tmp_path =
      std::string("/tmp/percentile_sampler_test_") +
      std::to_string(::getpid()) + ".json";
  {
    std::ofstream ofs(tmp_path);
    ofs << folly::toJson(arr);
  }
  PercentileSampler s;
  EXPECT_TRUE(s.load(tmp_path, "req_size"));
  EXPECT_EQ(s.numPoints(), 3u);
  EXPECT_EQ(s.valueAtProbability(0.5), 200);
  ::unlink(tmp_path.c_str());
}

} // namespace
} // namespace ranking
