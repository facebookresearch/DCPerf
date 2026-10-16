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

// TorchScriptOps program API, declared without including torch

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace facebook::cea::chips::adsim::torchscript_ops {

enum class OpKind {
  Copy,
  Fill,
  Add,
  LayerNorm,
  SearchSorted,
  ClampRangeLengths,
  CompleteCumsum,
  Diff,
  IndexSelect,
  Cat,
  ToHalf,
};

inline OpKind parseOpKind(const std::string& name) {
  static const std::vector<std::pair<std::string, OpKind>> kNames = {
      {"copy", OpKind::Copy},
      {"fill", OpKind::Fill},
      {"add", OpKind::Add},
      {"layernorm", OpKind::LayerNorm},
      {"searchsorted", OpKind::SearchSorted},
      {"clamp_range_lengths", OpKind::ClampRangeLengths},
      {"complete_cumsum", OpKind::CompleteCumsum},
      {"diff", OpKind::Diff},
      {"index_select", OpKind::IndexSelect},
      {"cat", OpKind::Cat},
      {"to_half", OpKind::ToHalf},
  };
  for (const auto& [n, kind] : kNames) {
    if (n == name) {
      return kind;
    }
  }
  throw std::invalid_argument("unknown TorchScriptOps op: " + name);
}

/* One op invocation on rows x cols elements; `aux` is the boundaries per row
 * (searchsorted), parts (cat), or table rows (index_select). */
struct OpSpec {
  OpKind kind;
  int64_t rows;
  int64_t cols;
  int64_t aux;
  int reps;
};

inline void checkOpSpec(const OpSpec& spec) {
  if (1 > spec.rows || 1 > spec.cols || 1 > spec.aux || 0 > spec.reps) {
    throw std::invalid_argument(
        "TorchScriptOps rows, cols and aux must be positive and reps "
        "non-negative");
  }
  if (OpKind::Diff == spec.kind && 2 > spec.rows * spec.cols) {
    throw std::invalid_argument(
        "TorchScriptOps diff needs at least 2 elements");
  }
}

/* Read-only inputs of one op, replicated `nbuffers` times. */
class OpInputs {
 public:
  OpInputs(const OpSpec& spec, int nbuffers, uint64_t seed)
      : spec_(spec), nbuffers_(std::max(1, nbuffers)) {
    checkOpSpec(spec);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    const int64_t n = spec.rows * spec.cols;
    stride_ = inputElems();
    f32_.resize(stride_ * nbuffers_);
    for (auto& v : f32_) {
      v = unit(rng);
    }
    switch (spec.kind) {
      case OpKind::SearchSorted:
        for (int b = 0; nbuffers_ > b; ++b) {
          float* bounds = f32_.data() + b * stride_ + n;
          for (int64_t r = 0; spec.rows > r; ++r) {
            std::sort(bounds + r * spec.aux, bounds + (r + 1) * spec.aux);
          }
        }
        break;
      case OpKind::ClampRangeLengths:
      case OpKind::CompleteCumsum:
        i32_.resize(2 * n * nbuffers_);
        for (auto& v : i32_) {
          v = static_cast<int32_t>(rng() % 64);
        }
        break;
      case OpKind::IndexSelect:
        index_.resize(spec.rows * nbuffers_);
        for (auto& v : index_) {
          v = static_cast<int64_t>(rng() % spec.aux);
        }
        break;
      case OpKind::Copy:
      case OpKind::Fill:
      case OpKind::Add:
      case OpKind::LayerNorm:
      case OpKind::Diff:
      case OpKind::Cat:
      case OpKind::ToHalf:
        break;
    }
  }

  const OpSpec& spec() const {
    return spec_;
  }
  int nbuffers() const {
    return nbuffers_;
  }
  const float* f32(int b) const {
    return f32_.data() + b * stride_;
  }
  const int32_t* i32(int b) const {
    return i32_.data() + b * 2 * spec_.rows * spec_.cols;
  }
  const int64_t* index(int b) const {
    return index_.data() + b * spec_.rows;
  }
  // Floats per buffer; LayerNorm's gamma and beta follow the input.
  int64_t inputElems() const {
    const int64_t n = spec_.rows * spec_.cols;
    switch (spec_.kind) {
      case OpKind::Add:
        return 2 * n;
      case OpKind::LayerNorm:
        return n + 2 * spec_.cols;
      case OpKind::SearchSorted:
        return n + spec_.rows * spec_.aux;
      case OpKind::IndexSelect:
        return spec_.aux * spec_.cols;
      case OpKind::Cat:
        return n * spec_.aux;
      case OpKind::ClampRangeLengths:
      case OpKind::CompleteCumsum:
        return 1;
      case OpKind::Copy:
      case OpKind::Fill:
      case OpKind::Diff:
      case OpKind::ToHalf:
        break;
    }
    return n;
  }

 private:
  OpSpec spec_;
  int nbuffers_;
  int64_t stride_ = 0;
  std::vector<float> f32_;
  std::vector<int32_t> i32_;
  std::vector<int64_t> index_;
};

/* Shape of a generated graph of small ops on rows x cols floats. */
struct GraphSpec {
  int nops = 0; // Op calls in the graph; 0 disables the graph.
  int nruns = 1; // Graph runs per fire.
  int nregs = 32; // Values kept live for later ops.
  int ninputs = 8;
  int64_t rows = 32;
  int64_t cols = 64;
  double zipf_s = 0.85; // Skew of op popularity.
  // Draw ops as shuffled passes over the templates instead of by popularity.
  bool cover = false;
  // Names of the templates to draw from; empty for all of templateNames().
  std::vector<std::string> templates;
  uint64_t seed = 1;
  // Graph copies to cycle through: "float32", "float64", "bfloat16", "float16"
  std::vector<std::string> dtypes = {"float32"};
};

/* A pointer chase through a shared table of 64-byte lines. */
struct ChaseSpec {
  int len = 0; // Hops per fire; 0 disables the chase.
  int64_t lines = 65536; // Table size in 64-byte lines.
  int width = 1; // Independent walks advanced by each hop.
  uint64_t seed = 1;
};

/* One thread's workspace and tensor handles for running a Program. */
class Frame {
 public:
  virtual ~Frame() = default;
  // Runs the ops on random buffers, then the graph, then the chase.
  virtual int64_t fire() = 0;
  // Runs every op on buffer `b`, without the graph.
  virtual int64_t fireBuffer(int b) = 0;
  // Bytes of op k's latest output.
  virtual std::vector<char> output(size_t k) const = 0;
};

class Program {
 public:
  virtual ~Program() = default;
  virtual std::unique_ptr<Frame> newFrame(uint64_t seed) const = 0;
  virtual std::string describe() const = 0;
  // TorchScript source of the program.
  virtual const std::string& source() const = 0;
};

/* Compile a Program and run it a few times to warm up the profiling
 * executor.
 *
 * @param  $specs  Operators to run per fire, in order
 * @param  $nbuffers  Copies of every operator's inputs
 * @param  $graph  Generated graph to run after the operators
 * @param  $chase  Pointer chase to run after the graph
 */
std::shared_ptr<Program> compileProgram(
    const std::vector<OpSpec>& specs,
    int nbuffers,
    const GraphSpec& graph,
    const ChaseSpec& chase = {});

/* Names of the op templates a generated graph draws from. */
std::vector<std::string> templateNames();

/* TorchScript source of the graph, one `graph_<dtype>(x, idx)` per dtype;
 * `returnAll` returns every value instead of the unread ones. */
std::string graphSource(const GraphSpec& graph, bool returnAll);

} // namespace facebook::cea::chips::adsim::torchscript_ops
