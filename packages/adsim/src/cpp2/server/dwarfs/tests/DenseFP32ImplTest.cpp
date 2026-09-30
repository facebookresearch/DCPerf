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

#include <cea/chips/adsim/cpp2/server/dwarfs/DenseFP32Impl.h>

#include <cstddef>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::dense_fp32 {
namespace {

using Matrix = std::vector<float>;

// Successive matrices of the given sizes from one U(-0.05, 0.05) stream
// seeded with 1.
std::vector<Matrix> seedOneStream(const std::vector<size_t>& sizes) {
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> dist(-0.05f, 0.05f);
  std::vector<Matrix> out;
  for (size_t n : sizes) {
    Matrix m(n);
    for (auto& v : m) {
      v = dist(rng);
    }
    out.push_back(std::move(m));
  }
  return out;
}

// Weight sizes of one set, layer by layer.
std::vector<size_t> layerSizes(const std::vector<int>& dims) {
  std::vector<size_t> sizes;
  for (size_t l = 0; dims.size() - 1 > l; ++l) {
    sizes.push_back(static_cast<size_t>(dims[l]) * dims[l + 1]);
  }
  return sizes;
}

// The row-major matrix held by a packed weight.
Matrix unpacked(fbgemm::PackedGemmMatrixFP32& w) {
  Matrix m(static_cast<size_t>(w.numRows()) * w.numCols());
  w.unpack(m.data(), fbgemm::matrix_op_t::NoTranspose);
  return m;
}

// The MLP computed with double accumulation, rounded to float per layer.
Matrix naiveMlp(
    Matrix x,
    int batch,
    const std::vector<int>& dims,
    const std::vector<Matrix>& weights) {
  for (size_t l = 0; weights.size() > l; ++l) {
    const int k = dims[l];
    const int n = dims[l + 1];
    Matrix y(static_cast<size_t>(batch) * n);
    for (int i = 0; batch > i; ++i) {
      for (int j = 0; n > j; ++j) {
        double acc = 0;
        for (int p = 0; k > p; ++p) {
          acc += double{x[i * k + p]} * weights[l][p * n + j];
        }
        y[i * n + j] = static_cast<float>(acc);
      }
    }
    x = std::move(y);
  }
  return x;
}

// forward()'s output as a vector.
Matrix runForward(const Layers& layers, int batch, int ncols, Workspace& ws) {
  const float* out = forward(layers, batch, ws);
  return Matrix(out, out + static_cast<size_t>(batch) * ncols);
}

TEST(DenseFP32Test, WeightSetsContinueOneStream) {
  const std::vector<int> dims{3, 4, 2};
  const auto expected = seedOneStream({12, 8, 12, 8});

  auto sets = makeWeightSets(dims, 2);

  ASSERT_EQ(sets.size(), 2);
  ASSERT_EQ(sets[0].size(), 2);
  ASSERT_EQ(sets[1].size(), 2);
  const std::vector<Matrix> actual{
      unpacked(*sets[0][0]),
      unpacked(*sets[0][1]),
      unpacked(*sets[1][0]),
      unpacked(*sets[1][1])};
  EXPECT_EQ(actual, expected);
}

TEST(DenseFP32Test, ForwardMatchesNaiveMlp) {
  // Widths that are not multiples of the kernel's column block.
  const int batch = 5;
  const std::vector<int> dims{37, 50, 19};
  Workspace ws;
  ws.input.resize(static_cast<size_t>(batch) * dims[0]);
  for (size_t i = 0; ws.input.size() > i; ++i) {
    ws.input[i] =
        static_cast<float>(static_cast<int>(i * 37 % 101) - 50) * 0.001f;
  }
  const Matrix input = ws.input;
  const auto sets = makeWeightSets(dims, 1);

  const Matrix actual = runForward(sets[0], batch, dims.back(), ws);

  EXPECT_THAT(
      actual,
      testing::Pointwise(
          testing::FloatNear(1e-6f),
          naiveMlp(input, batch, dims, seedOneStream(layerSizes(dims)))));
}

TEST(DenseFP32Test, ForwardMatchesNaiveMlpForDefaultInput) {
  const int batch = 16;
  const std::vector<int> dims{2048, 1024, 512, 256};
  const auto sets = makeWeightSets(dims, 1);
  Workspace ws;

  const Matrix actual = runForward(sets[0], batch, dims.back(), ws);

  const Matrix input(static_cast<size_t>(batch) * dims[0], 0.01f);
  EXPECT_THAT(
      actual,
      testing::Pointwise(
          testing::FloatNear(1e-6f),
          naiveMlp(input, batch, dims, seedOneStream(layerSizes(dims)))));
}

TEST(DenseFP32Test, PassesRepeatAndKeepInput) {
  // Two and three layers: the last layer writes each work buffer in turn.
  for (const std::vector<int>& dims :
       {std::vector<int>{24, 16, 8}, std::vector<int>{24, 16, 8, 4}}) {
    const int batch = 3;
    const auto sets = makeWeightSets(dims, 1);
    Workspace ws;
    const Matrix first = runForward(sets[0], batch, dims.back(), ws);
    const Matrix input = ws.input;

    const Matrix second = runForward(sets[0], batch, dims.back(), ws);

    EXPECT_EQ(second, first) << dims.size() - 1 << " layers";
    EXPECT_EQ(ws.input, input) << dims.size() - 1 << " layers";
  }
}

TEST(DenseFP32Test, WorkspaceGrowsForLargerShapes) {
  const std::vector<int> small{4, 3};
  const std::vector<int> large{16, 32, 8};
  const auto smallSets = makeWeightSets(small, 1);
  const auto largeSets = makeWeightSets(large, 1);
  Workspace fresh;
  const Matrix expected = runForward(largeSets[0], 5, large.back(), fresh);
  Workspace ws;
  runForward(smallSets[0], 2, small.back(), ws);

  const Matrix actual = runForward(largeSets[0], 5, large.back(), ws);

  EXPECT_EQ(actual, expected);
}

TEST(DenseFP32Test, ValidateRejectsInvalidShapes) {
  EXPECT_NO_THROW(validate(1, {1, 1}, 1, 1));
  EXPECT_THROW(validate(0, {4, 4}, 1, 1), std::invalid_argument);
  EXPECT_THROW(validate(1, {4}, 1, 1), std::invalid_argument);
  EXPECT_THROW(validate(1, {4, 0, 4}, 1, 1), std::invalid_argument);
  EXPECT_THROW(validate(1, {4, 4}, 0, 1), std::invalid_argument);
  EXPECT_THROW(validate(1, {4, 4}, 1, 0), std::invalid_argument);
}

TEST(DenseFP32Test, ValidateShapeRejectsOtherShapes) {
  const std::vector<Layers> sets = makeWeightSets({8, 4, 2}, 2);

  EXPECT_NO_THROW(validateShape(sets, {8, 4, 2}, 2));
  EXPECT_THROW(validateShape(sets, {8, 4, 2}, 1), std::invalid_argument);
  EXPECT_THROW(validateShape(sets, {8, 4, 2}, 3), std::invalid_argument);
  EXPECT_THROW(validateShape(sets, {8, 4}, 2), std::invalid_argument);
  EXPECT_THROW(validateShape(sets, {8, 4, 2, 1}, 2), std::invalid_argument);
  EXPECT_THROW(validateShape(sets, {8, 5, 2}, 2), std::invalid_argument);
}

} // namespace
} // namespace facebook::cea::chips::adsim::dense_fp32
