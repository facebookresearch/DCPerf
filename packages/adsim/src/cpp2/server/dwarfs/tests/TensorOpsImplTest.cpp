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

#include <cea/chips/adsim/cpp2/server/dwarfs/TensorOpsImpl.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::tensor_ops {
namespace {

TEST(TensorOpsImplTest, ParsesOpNames) {
  const std::vector<OpKind> expected = {
      OpKind::Copy,
      OpKind::Fill,
      OpKind::Add,
      OpKind::LayerNorm,
      OpKind::SearchSorted,
      OpKind::ClampRangeLengths,
      OpKind::CompleteCumsum,
      OpKind::Diff,
      OpKind::IndexSelect,
      OpKind::Cat,
      OpKind::ToHalf,
  };
  std::vector<OpKind> parsed;
  for (const char* name :
       {"copy",
        "fill",
        "add",
        "layernorm",
        "searchsorted",
        "clamp_range_lengths",
        "complete_cumsum",
        "diff",
        "index_select",
        "cat",
        "to_half"}) {
    parsed.push_back(parseOpKind(name));
  }
  EXPECT_EQ(parsed, expected);
  EXPECT_THROW(parseOpKind("matmul"), std::invalid_argument);
}

TEST(TensorOpsImplTest, ElementwiseKernels) {
  const std::vector<float> a = {1.f, 2.f, 3.f};
  const std::vector<float> b = {2.f, 4.f, 6.f};
  std::vector<float> out(3);

  copyKernel(out.data(), a.data(), 3);
  EXPECT_EQ(out, a);
  fillKernel(out.data(), 7.f, 3);
  EXPECT_EQ(out, std::vector<float>({7.f, 7.f, 7.f}));
  addKernel(out.data(), a.data(), b.data(), 0.5f, 3);
  EXPECT_EQ(out, std::vector<float>({2.f, 4.f, 6.f}));
}

TEST(TensorOpsImplTest, LayerNormMatchesReference) {
  // 19 columns cover the 8-wide vector body and the scalar tail.
  constexpr int64_t kRows = 3;
  constexpr int64_t kCols = 19;
  constexpr float kEps = 1e-5f;
  std::vector<float> in(kRows * kCols);
  std::vector<float> gamma(kCols);
  std::vector<float> beta(kCols);
  for (int64_t i = 0; kRows * kCols > i; ++i) {
    in[i] = static_cast<float>((i * 7) % 11) - 3.f;
  }
  for (int64_t c = 0; kCols > c; ++c) {
    gamma[c] = 1.f + 0.1f * c;
    beta[c] = 0.5f - 0.05f * c;
  }

  std::vector<float> out(kRows * kCols);
  layerNormKernel(
      out.data(), in.data(), gamma.data(), beta.data(), kRows, kCols, kEps);

  for (int64_t r = 0; kRows > r; ++r) {
    double mean = 0.0;
    for (int64_t c = 0; kCols > c; ++c) {
      mean += in[r * kCols + c];
    }
    mean /= kCols;
    double var = 0.0;
    for (int64_t c = 0; kCols > c; ++c) {
      const double d = in[r * kCols + c] - mean;
      var += d * d;
    }
    var /= kCols;
    for (int64_t c = 0; kCols > c; ++c) {
      const double expected =
          (in[r * kCols + c] - mean) / std::sqrt(var + kEps) * gamma[c] +
          beta[c];
      EXPECT_NEAR(out[r * kCols + c], expected, 1e-4) << r << "," << c;
    }
  }
}

TEST(TensorOpsImplTest, SearchSortedFindsLeftInsertionPoint) {
  const std::vector<float> boundaries = {
      1.f, 3.f, 5.f, 7.f, 0.f, 0.f, 2.f, 2.f};
  const std::vector<float> values = {0.f, 3.f, 8.f, 0.f, 1.f, 2.f};
  std::vector<int64_t> out(6);

  searchSortedKernel<float>(
      out.data(), boundaries.data(), values.data(), 2, 4, 3);

  EXPECT_EQ(out, std::vector<int64_t>({0, 1, 4, 0, 2, 2}));
}

TEST(TensorOpsImplTest, ClampRangeLengthsKeepsOffsets) {
  const std::vector<int32_t> ranges = {0, 5, 5, 20, 25, 16};
  std::vector<int32_t> out(6);

  clampRangeLengthsKernel(out.data(), ranges.data(), 3, 16);

  EXPECT_EQ(out, std::vector<int32_t>({0, 5, 5, 16, 25, 16}));
}

TEST(TensorOpsImplTest, CompleteCumsumStartsAtZero) {
  const std::vector<int32_t> lengths = {3, 0, 2};
  std::vector<int64_t> out(4);

  completeCumsumKernel(out.data(), lengths.data(), 3);

  EXPECT_EQ(out, std::vector<int64_t>({0, 3, 3, 5}));
}

TEST(TensorOpsImplTest, Diff) {
  const std::vector<float> in = {1.f, 4.f, 9.f, 16.f};
  std::vector<float> out(3);

  diffKernel(out.data(), in.data(), 4);

  EXPECT_EQ(out, std::vector<float>({3.f, 5.f, 7.f}));
}

TEST(TensorOpsImplTest, IndexSelectGathersRows) {
  const std::vector<float> table = {0.f, 1.f, 10.f, 11.f, 20.f, 21.f};
  const std::vector<int64_t> index = {2, 0, 2};
  std::vector<float> out(6);

  indexSelectKernel(out.data(), table.data(), index.data(), 3, 2);

  EXPECT_EQ(out, std::vector<float>({20.f, 21.f, 0.f, 1.f, 20.f, 21.f}));
}

TEST(TensorOpsImplTest, CatJoinsPartsAlongColumns) {
  // Two parts of 2 x 2: {a, b; c, d} and {e, f; g, h}.
  const std::vector<float> in = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
  std::vector<float> out(8);

  catKernel(out.data(), in.data(), 2, 2, 2);

  EXPECT_EQ(out, std::vector<float>({1.f, 2.f, 5.f, 6.f, 3.f, 4.f, 7.f, 8.f}));
}

TEST(TensorOpsImplTest, ToHalfProducesIeeeBits) {
  // The last three values go through the scalar tail.
  const std::vector<float> in = {
      0.f,
      -0.f,
      1.f,
      -2.f,
      0.5f,
      65504.f,
      1.f / 3.f,
      std::ldexp(1.f, -14),
      0.1f,
      70000.f,
      std::ldexp(1.f, -24),
  };
  std::vector<uint16_t> out(in.size());

  toHalfKernel(out.data(), in.data(), static_cast<int64_t>(in.size()));

  const std::vector<uint16_t> expected = {
      0x0000,
      0x8000,
      0x3c00,
      0xc000,
      0x3800,
      0x7bff,
      0x3555,
      0x0400,
      0x2e66,
      0x7c00,
      0x0001,
  };
  EXPECT_EQ(out, expected);
}

TEST(TensorOpsImplTest, RunOpStaysWithinOutputWorkspace) {
  for (const char* name :
       {"copy",
        "fill",
        "add",
        "layernorm",
        "searchsorted",
        "clamp_range_lengths",
        "complete_cumsum",
        "diff",
        "index_select",
        "cat",
        "to_half"}) {
    const OpSpec spec{parseOpKind(name), 4, 13, 5, 1};
    const OpInputs inputs(spec, 2, /*seed=*/1);
    // A guard word after the workspace detects writes past outputBytes().
    constexpr unsigned char kGuard = 0xa5;
    std::vector<unsigned char> out(inputs.outputBytes() + 1, kGuard);

    for (int b = 0; inputs.nbuffers() > b; ++b) {
      EXPECT_TRUE(std::isfinite(runOp(inputs, b, out.data()))) << name;
    }
    EXPECT_EQ(out.back(), kGuard) << name;
  }
}

TEST(TensorOpsImplTest, InputsDependOnlyOnSeed) {
  const OpSpec spec{OpKind::Add, 4, 8, 1, 1};
  const OpInputs first(spec, 2, /*seed=*/3);
  const OpInputs second(spec, 2, /*seed=*/3);
  const int64_t n = first.inputElems();

  for (int b = 0; 2 > b; ++b) {
    EXPECT_EQ(
        std::vector<float>(first.f32(b), first.f32(b) + n),
        std::vector<float>(second.f32(b), second.f32(b) + n));
  }
}

TEST(TensorOpsImplTest, RejectsInvalidShapes) {
  EXPECT_THROW(
      OpInputs(OpSpec{OpKind::Copy, 0, 8, 1, 1}, 1, 1), std::invalid_argument);
  EXPECT_THROW(
      OpInputs(OpSpec{OpKind::Cat, 4, 8, 0, 1}, 1, 1), std::invalid_argument);
  EXPECT_THROW(
      OpInputs(OpSpec{OpKind::Diff, 1, 1, 1, 1}, 1, 1), std::invalid_argument);
}

} // namespace
} // namespace facebook::cea::chips::adsim::tensor_ops
