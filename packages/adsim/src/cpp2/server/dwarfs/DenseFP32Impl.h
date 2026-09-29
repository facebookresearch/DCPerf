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

#pragma once

// An fp32 MLP forward pass: a chain of dense layers, each an fp32 GEMM of
// `batch` activation rows against a prepacked weight matrix, with no bias or
// activation function. The code depends only on fbgemm so it can be tested
// without AdSim.

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <fbgemm/FbgemmFP32.h>

namespace facebook::cea::chips::adsim::dense_fp32 {

// One weight set: layer l holds a dims[l] x dims[l + 1] packed matrix.
using Layers = std::vector<std::unique_ptr<fbgemm::PackedGemmMatrixFP32>>;

/* Throws std::invalid_argument unless there are at least two dims and every
 * size is positive.
 */
inline void validate(
    int batch,
    const std::vector<int>& dims,
    int nweight_sets,
    int niters) {
  if (2 > dims.size()) {
    throw std::invalid_argument("DenseFP32 needs at least two dims");
  }
  if (1 > *std::min_element(dims.begin(), dims.end())) {
    throw std::invalid_argument("DenseFP32 dims must be positive");
  }
  if (1 > batch || 1 > nweight_sets || 1 > niters) {
    throw std::invalid_argument(
        "DenseFP32 batch, nweight_sets, and niters must be positive, got " +
        std::to_string(batch) + ", " + std::to_string(nweight_sets) + ", " +
        std::to_string(niters));
  }
}

/* A rows x cols row-major matrix drawn from U(-0.05, 0.05). */
inline std::vector<float> randomMatrix(std::mt19937& rng, int rows, int cols) {
  std::uniform_real_distribution<float> dist(-0.05f, 0.05f);
  std::vector<float> m(static_cast<size_t>(rows) * cols);
  for (auto& v : m) {
    v = dist(rng);
  }
  return m;
}

/* Packs `nsets` weight sets for the layer widths `dims`. All sets come from
 * one generator seeded with 1, drawn set by set and layer by layer, so the
 * weights depend only on `dims` and `nsets`.
 */
inline std::vector<Layers> makeWeightSets(
    const std::vector<int>& dims,
    int nsets) {
  std::vector<Layers> sets(nsets);
  std::mt19937 rng(1);
  for (auto& set : sets) {
    for (size_t l = 0; dims.size() - 1 > l; ++l) {
      const std::vector<float> w = randomMatrix(rng, dims[l], dims[l + 1]);
      set.push_back(
          std::make_unique<fbgemm::PackedGemmMatrixFP32>(
              fbgemm::matrix_op_t::NoTranspose,
              dims[l],
              dims[l + 1],
              1.f,
              w.data()));
    }
  }
  return sets;
}

/* Throws std::invalid_argument unless `sets` holds `nsets` weight sets whose
 * layers have the widths in `dims`. Kernels that share weights through one
 * input label must agree on their shape.
 */
inline void validateShape(
    const std::vector<Layers>& sets,
    const std::vector<int>& dims,
    int nsets) {
  bool same = static_cast<size_t>(nsets) == sets.size();
  for (size_t s = 0; same && sets.size() > s; ++s) {
    same = dims.size() - 1 == sets[s].size();
    for (size_t l = 0; same && sets[s].size() > l; ++l) {
      same = dims[l] == sets[s][l]->numRows() &&
          dims[l + 1] == sets[s][l]->numCols();
    }
  }
  if (!same) {
    throw std::invalid_argument(
        "DenseFP32 kernels sharing an input label need the same dims and "
        "nweight_sets");
  }
}

/* Buffers for forward(). The first layer reads `input`, which forward()
 * never writes, so every pass computes the same values; later layers
 * alternate between the two `act` buffers. The buffers only grow.
 */
struct Workspace {
  std::vector<float> input;
  std::array<std::vector<float>, 2> act;

  void fit(const Layers& layers, int batch) {
    const size_t in = static_cast<size_t>(batch) * layers[0]->numRows();
    if (input.size() < in) {
      input.assign(in, 0.01f);
    }
    int width = 0;
    for (const auto& w : layers) {
      width = std::max(width, w->numCols());
    }
    const size_t out = static_cast<size_t>(batch) * width;
    for (auto& a : act) {
      if (a.size() < out) {
        a.assign(out, 0.f);
      }
    }
  }
};

/* Runs `batch` rows of `ws.input` through `layers` and returns the
 * batch x layers.back()->numCols() output, which lives in `ws`.
 */
inline const float* forward(const Layers& layers, int batch, Workspace& ws) {
  ws.fit(layers, batch);
  const float* in = ws.input.data();
  for (size_t l = 0; layers.size() > l; ++l) {
    float* out = ws.act[l % 2].data();
    fbgemm::cblas_gemm_compute(
        fbgemm::matrix_op_t::NoTranspose, batch, in, *layers[l], 0.f, out);
    in = out;
  }
  return in;
}

} // namespace facebook::cea::chips::adsim::dense_fp32
