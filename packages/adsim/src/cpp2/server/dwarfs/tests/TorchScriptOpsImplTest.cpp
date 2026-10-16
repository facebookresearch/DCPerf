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

#include <cea/chips/adsim/cpp2/server/dwarfs/TorchScriptOpsImpl.h>

#include <c10/core/impl/LocalDispatchKeySet.h>
#include <c10/util/Half.h>
#include <torch/library.h>
#include <torch/script.h>
#include <torch/torch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::torchscript_ops {
namespace {

const std::vector<std::string> kDTypes = {
    "float32",
    "float64",
    "bfloat16",
    "float16"};

// One op of every kind
std::vector<OpSpec> everyOp() {
  return {
      {OpKind::SearchSorted, 64, 16, 64, 2},
      {OpKind::ClampRangeLengths, 512, 64, 4, 2},
      {OpKind::CompleteCumsum, 512, 64, 4, 2},
      {OpKind::Diff, 512, 256, 4, 2},
      {OpKind::IndexSelect, 256, 256, 1024, 2},
      {OpKind::Cat, 256, 64, 4, 2},
      {OpKind::Copy, 1024, 256, 4, 2},
      {OpKind::Fill, 1024, 256, 4, 2},
      {OpKind::LayerNorm, 512, 256, 4, 2},
      {OpKind::Add, 512, 256, 4, 2},
      {OpKind::ToHalf, 512, 256, 4, 2},
  };
}

int64_t totalReps(const std::vector<OpSpec>& specs) {
  int64_t n = 0;
  for (const auto& s : specs) {
    n += s.reps;
  }
  return n;
}

template <typename T>
std::vector<char> bytesOf(const std::vector<T>& v) {
  const auto* p = reinterpret_cast<const char*>(v.data());
  return std::vector<char>(p, p + v.size() * sizeof(T));
}

// Op k's output for buffer b, computed with scalar loops
std::vector<char> expectedOutput(
    const std::vector<OpSpec>& specs,
    int nbuffers,
    size_t k,
    int b) {
  const OpInputs in(specs[k], nbuffers, /*seed=*/k + 1);
  const OpSpec& s = specs[k];
  const int64_t n = s.rows * s.cols;
  const float* x = in.f32(b);
  switch (s.kind) {
    case OpKind::Copy:
      return bytesOf(std::vector<float>(x, x + n));
    case OpKind::Fill:
      return bytesOf(std::vector<float>(n, x[0]));
    case OpKind::Add: {
      std::vector<float> y(n);
      for (int64_t i = 0; n > i; ++i) {
        y[i] = x[i] + 0.5f * x[n + i];
      }
      return bytesOf(y);
    }
    case OpKind::LayerNorm: {
      std::vector<float> y(n);
      const float* gamma = x + n;
      const float* beta = x + n + s.cols;
      for (int64_t r = 0; s.rows > r; ++r) {
        const float* row = x + r * s.cols;
        double mean = 0;
        for (int64_t c = 0; s.cols > c; ++c) {
          mean += row[c];
        }
        mean /= s.cols;
        double var = 0;
        for (int64_t c = 0; s.cols > c; ++c) {
          var += (row[c] - mean) * (row[c] - mean);
        }
        const double rstd = 1.0 / std::sqrt(var / s.cols + 1e-5);
        for (int64_t c = 0; s.cols > c; ++c) {
          y[r * s.cols + c] =
              static_cast<float>((row[c] - mean) * rstd * gamma[c] + beta[c]);
        }
      }
      return bytesOf(y);
    }
    case OpKind::SearchSorted: {
      std::vector<int64_t> y(n);
      for (int64_t r = 0; s.rows > r; ++r) {
        const float* bounds = x + n + r * s.aux;
        for (int64_t c = 0; s.cols > c; ++c) {
          y[r * s.cols + c] =
              std::lower_bound(bounds, bounds + s.aux, x[r * s.cols + c]) -
              bounds;
        }
      }
      return bytesOf(y);
    }
    case OpKind::ClampRangeLengths: {
      std::vector<int32_t> y(in.i32(b), in.i32(b) + 2 * n);
      for (int64_t i = 0; n > i; ++i) {
        y[2 * i + 1] = std::min(y[2 * i + 1], 16);
      }
      return bytesOf(y);
    }
    case OpKind::CompleteCumsum: {
      std::vector<int64_t> y(n + 1, 0);
      for (int64_t i = 0; n > i; ++i) {
        y[i + 1] = y[i] + in.i32(b)[i];
      }
      return bytesOf(y);
    }
    case OpKind::Diff: {
      std::vector<float> y(n - 1);
      for (int64_t i = 0; n - 1 > i; ++i) {
        y[i] = x[i + 1] - x[i];
      }
      return bytesOf(y);
    }
    case OpKind::IndexSelect: {
      std::vector<float> y(n);
      for (int64_t r = 0; s.rows > r; ++r) {
        std::copy_n(x + in.index(b)[r] * s.cols, s.cols, y.data() + r * s.cols);
      }
      return bytesOf(y);
    }
    case OpKind::Cat: {
      std::vector<float> y(n * s.aux);
      for (int64_t r = 0; s.rows > r; ++r) {
        for (int64_t q = 0; s.aux > q; ++q) {
          std::copy_n(
              x + q * n + r * s.cols,
              s.cols,
              y.data() + (r * s.aux + q) * s.cols);
        }
      }
      return bytesOf(y);
    }
    case OpKind::ToHalf: {
      std::vector<uint16_t> y(n);
      for (int64_t i = 0; n > i; ++i) {
        y[i] = c10::Half(x[i]).x;
      }
      return bytesOf(y);
    }
  }
  return {};
}

// Compare bit for bit, LayerNorm within 1e-4
bool matches(
    OpKind kind,
    const std::vector<char>& got,
    const std::vector<char>& want) {
  if (got.size() != want.size()) {
    return false;
  }
  if (OpKind::LayerNorm != kind) {
    return 0 == std::memcmp(got.data(), want.data(), got.size());
  }
  const auto* g = reinterpret_cast<const float*>(got.data());
  const auto* w = reinterpret_cast<const float*>(want.data());
  for (size_t i = 0; got.size() / sizeof(float) > i; ++i) {
    if (!(1e-4 > std::abs(g[i] - w[i]))) {
      return false;
    }
  }
  return true;
}

at::ScalarType scalarType(const std::string& dtype) {
  if ("float64" == dtype) {
    return at::kDouble;
  }
  if ("bfloat16" == dtype) {
    return at::kBFloat16;
  }
  if ("float16" == dtype) {
    return at::kHalf;
  }
  return at::kFloat;
}

// Inputs of a generated graph: the values, a matrix, and sorted boundaries
c10::List<at::Tensor> graphInputs(
    const std::vector<at::Tensor>& values,
    int64_t cols,
    at::ScalarType type) {
  c10::List<at::Tensor> inputs;
  for (const auto& x : values) {
    inputs.push_back(x.to(type));
  }
  inputs.push_back(((at::rand({cols, cols}) - 0.5) * 0.25).to(type));
  inputs.push_back(at::linspace(-2.0, 2.0, 9).to(type));
  return inputs;
}

struct AtenCall {
  std::string name;
  std::vector<c10::IValue> inputs;
};

thread_local std::vector<AtenCall>* tCalls = nullptr;

void recordCall(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  const auto nargs = static_cast<int64_t>(op.schema().arguments().size());
  tCalls->push_back({op.schema().name(), {stack->end() - nargs, stack->end()}});
  c10::impl::ExcludeDispatchKeyGuard guard(
      c10::DispatchKey::TESTING_ONLY_GenericMode);
  op.callBoxed(stack);
}

// Record the top-level ATen calls `f` makes on this thread
std::vector<AtenCall> atenCalls(const std::function<void()>& f) {
  std::vector<AtenCall> calls;
  tCalls = &calls;
  {
    auto m = MAKE_TORCH_LIBRARY_IMPL(_, TESTING_ONLY_GenericMode);
    m.fallback(torch::CppFunction::makeFromBoxedFunction<&recordCall>());
    c10::impl::IncludeDispatchKeyGuard guard(
        c10::DispatchKey::TESTING_ONLY_GenericMode);
    f();
  }
  tCalls = nullptr;
  return calls;
}

// Count calls of op `name` on a tensor
int64_t tensorCalls(
    const std::vector<AtenCall>& calls,
    const std::string& name) {
  return std::count_if(calls.begin(), calls.end(), [&](const AtenCall& c) {
    return name == c.name && !c.inputs.empty() && c.inputs[0].isTensor();
  });
}

void expectRejected(const std::function<void()>& f, const std::string& what) {
  try {
    f();
    ADD_FAILURE() << "accepted; expected an error with \"" << what << "\"";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find(what), std::string::npos) << e.what();
  }
}

TEST(TorchScriptOpsImplTest, OpsMatchReference) {
  const auto specs = everyOp();
  const int nbuffers = 4;
  auto frame = compileProgram(specs, nbuffers, {})->newFrame(1);
  for (int b : {0, nbuffers - 1}) {
    EXPECT_EQ(frame->fireBuffer(b), totalReps(specs));
    for (size_t k = 0; specs.size() > k; ++k) {
      EXPECT_TRUE(matches(
          specs[k].kind,
          frame->output(k),
          expectedOutput(specs, nbuffers, k, b)))
          << "buffer " << b << ", op " << k;
    }
  }
}

// A fire runs and counts every op rep and chase hop
TEST(TorchScriptOpsImplTest, FireRunsEveryRepAndHop) {
  const std::vector<OpSpec> specs = {
      {OpKind::Add, 16, 16, 4, 3}, {OpKind::Diff, 16, 16, 4, 2}};
  ChaseSpec chase;
  chase.len = 50;
  chase.lines = 256;
  auto frame = compileProgram(specs, 2, {}, chase)->newFrame(1);
  int64_t n = 0;
  const auto calls = atenCalls([&]() { n = frame->fire(); });
  EXPECT_EQ(tensorCalls(calls, "aten::add"), specs[0].reps);
  EXPECT_EQ(tensorCalls(calls, "aten::diff"), specs[1].reps);
  EXPECT_EQ(tensorCalls(calls, "aten::index_select"), chase.len);
  EXPECT_EQ(n, totalReps(specs) + chase.len);

  GraphSpec graph;
  graph.nops = 20;
  graph.rows = 4;
  graph.cols = 16;
  // Each graph run adds the number of values it returns, at least one.
  EXPECT_GE(
      compileProgram(specs, 2, graph, chase)->newFrame(1)->fire(),
      totalReps(specs) + chase.len + 1);
}

// The chase walks a single cycle through every 64-byte line of its table.
TEST(TorchScriptOpsImplTest, ChaseWalksEveryLine) {
  constexpr int kLines = 64;
  ChaseSpec chase;
  chase.lines = kLines;
  chase.len = 2 * kLines;
  auto frame = compileProgram({}, 1, {}, chase)->newFrame(1);
  std::vector<int64_t> at; // Table index each hop reads.
  for (const auto& c : atenCalls([&]() { frame->fire(); })) {
    if ("aten::index_select" == c.name) {
      at.push_back(c.inputs[2].toTensor().item<int64_t>());
    }
  }
  ASSERT_EQ(static_cast<int64_t>(at.size()), chase.len);
  const int64_t lineInts = 64 / sizeof(int64_t);
  std::set<int64_t> lines;
  for (int64_t i = 0; chase.lines > i; ++i) {
    EXPECT_EQ(at[i] % lineInts, 0) << "hop " << i;
    EXPECT_EQ(at[i + chase.lines], at[i]) << "hop " << i;
    lines.insert(at[i] / lineInts);
  }
  EXPECT_EQ(static_cast<int64_t>(lines.size()), chase.lines);
  EXPECT_GE(*lines.begin(), 0);
  EXPECT_LT(*lines.rbegin(), chase.lines);
}

// Each graph run calls each op once, cycling through the dtypes
TEST(TorchScriptOpsImplTest, GraphRunsEveryOpInEachDType) {
  GraphSpec graph;
  graph.nops = 8;
  graph.rows = 4;
  graph.cols = 16;
  // Elementwise templates without casts, matrices, boundaries, or indices
  graph.templates = {"relu", "sigmoid", "tanh", "abs"};
  graph.dtypes = kDTypes;
  const std::set<std::string> ops = {
      "aten::relu", "aten::sigmoid", "aten::tanh", "aten::abs"};
  auto frame = compileProgram({}, 1, graph)->newFrame(1);
  std::set<at::ScalarType> seen;
  for (size_t i = 0; kDTypes.size() > i; ++i) {
    int64_t nops = 0;
    std::set<at::ScalarType> types;
    for (const auto& c : atenCalls([&]() { frame->fire(); })) {
      nops += ops.count(c.name);
      for (const auto& v : c.inputs) {
        if (v.isTensor()) {
          types.insert(v.toTensor().scalar_type());
        }
      }
    }
    EXPECT_EQ(nops, graph.nops) << "run " << i;
    EXPECT_EQ(types.size(), size_t{1}) << "run " << i;
    seen.insert(types.begin(), types.end());
  }
  std::set<at::ScalarType> all;
  for (const auto& dtype : kDTypes) {
    all.insert(scalarType(dtype));
  }
  EXPECT_EQ(seen, all);
}

TEST(TorchScriptOpsImplTest, RejectsBadSpecs) {
  GraphSpec graph;
  graph.nops = 10;
  graph.rows = 4;
  graph.cols = 16;
  auto withGraph = [](const GraphSpec& g) {
    return [g]() { compileProgram({}, 1, g); };
  };

  GraphSpec g = graph;
  g.cols = 18;
  expectRejected(withGraph(g), "cols a positive multiple of 4");
  g = graph;
  g.rows = 0;
  expectRejected(withGraph(g), "positive nops, ninputs, nregs, and rows");
  g = graph;
  g.ninputs = 0;
  expectRejected(withGraph(g), "positive nops, ninputs, nregs, and rows");
  g = graph;
  g.nregs = 0;
  expectRejected(withGraph(g), "positive nops, ninputs, nregs, and rows");
  g = graph;
  g.nruns = 0;
  expectRejected(withGraph(g), "graph needs positive nruns");
  g = graph;
  g.dtypes = {"int32"};
  expectRejected(withGraph(g), "dtype int32 is not");
  g = graph;
  g.dtypes = {"float16", "float16"};
  expectRejected(withGraph(g), "dtype float16 is listed twice");
  g = graph;
  g.dtypes = {};
  expectRejected(withGraph(g), "needs a dtype");
  g = graph;
  g.templates = {"no_such_template"};
  expectRejected(withGraph(g), "template no_such_template is unknown");
  g = graph;
  g.templates = {templateNames()[0], templateNames()[0]};
  expectRejected(withGraph(g), "lists a template twice");

  expectRejected(
      []() { compileProgram({}, 1, {}); }, "needs ops, a graph, or a chase");
  auto withChase = [](const ChaseSpec& c) {
    return [c]() { compileProgram({}, 1, {}, c); };
  };
  ChaseSpec chase;
  chase.len = 10;
  chase.lines = 0;
  expectRejected(withChase(chase), "chase needs positive lines and width");
  chase.lines = 64;
  chase.width = 0;
  expectRejected(withChase(chase), "chase needs positive lines and width");
}

TEST(TorchScriptOpsImplTest, GraphSourceIsDeterministicInSeed) {
  GraphSpec g;
  g.nops = 50;
  g.rows = 4;
  g.cols = 16;
  const std::string source = graphSource(g, /*returnAll=*/false);
  EXPECT_EQ(graphSource(g, false), source);
  g.seed = 2;
  EXPECT_NE(graphSource(g, false), source);
}

// Graph values keep their dtype and stay finite
TEST(TorchScriptOpsImplTest, GraphValuesStayFiniteAndTyped) {
  c10::InferenceMode guard;
  GraphSpec g;
  g.rows = 4;
  g.cols = 16;
  g.dtypes = kDTypes;
  std::vector<at::Tensor> values;
  for (int i = 0; g.ninputs > i; ++i) {
    values.push_back(at::rand({g.rows, g.cols}));
  }
  const auto idx = at::randint(g.rows, {g.rows}, at::kLong);
  for (bool cover : {false, true}) {
    g.cover = cover;
    g.nops = cover ? static_cast<int>(templateNames().size()) : 200;
    auto cu = torch::jit::compile(graphSource(g, /*returnAll=*/true));
    for (const auto& dtype : kDTypes) {
      const auto type = scalarType(dtype);
      const auto inputs = graphInputs(values, g.cols, type);
      auto& fn = cu->get_function("graph_" + dtype);
      std::vector<at::Tensor> outs;
      for (int i = 0; 3 > i; ++i) {
        outs = fn({inputs, idx}).toTensorVector();
      }
      ASSERT_FALSE(outs.empty());
      for (size_t v = 0; outs.size() > v; ++v) {
        EXPECT_EQ(outs[v].scalar_type(), type)
            << dtype << (cover ? " cover" : "") << ", value " << v;
        EXPECT_TRUE(torch::isfinite(outs[v]).all().item<bool>())
            << dtype << (cover ? " cover" : "") << ", value " << v;
      }
    }
  }
}

// Every template keeps its shape and dtype and stays finite in every dtype
TEST(TorchScriptOpsImplTest, EveryTemplateKeepsShapeAndDType) {
  c10::InferenceMode guard;
  const int64_t rows = 4;
  const int64_t cols = 16;
  std::vector<at::Tensor> values;
  for (int i = 0; 8 > i; ++i) {
    values.push_back(at::rand({rows, cols}) * 8.0 - 4.0);
  }
  const auto idx = at::randint(rows, {rows}, at::kLong);
  for (const auto& dtype : kDTypes) {
    const auto type = scalarType(dtype);
    const auto inputs = graphInputs(values, cols, type);
    const bool lowp = at::kBFloat16 == type || at::kHalf == type;
    for (const auto& name : templateNames()) {
      GraphSpec g;
      g.nops = 1;
      g.rows = rows;
      g.cols = cols;
      g.cover = true;
      g.templates = {name};
      g.dtypes = {dtype};
      std::string source;
      try {
        source = graphSource(g, /*returnAll=*/true);
      } catch (const std::invalid_argument& e) {
        EXPECT_TRUE(lowp) << name << " " << dtype << ": " << e.what();
        EXPECT_NE(
            std::string(e.what()).find("has no templates for"),
            std::string::npos)
            << e.what();
        continue;
      }
      const auto v = torch::jit::compile(source)
                         ->get_function("graph_" + dtype)({inputs, idx})
                         .toTensorVector()
                         .back();
      EXPECT_EQ(v.sizes(), at::IntArrayRef({rows, cols}))
          << name << " " << dtype;
      EXPECT_EQ(v.scalar_type(), type) << name << " " << dtype;
      EXPECT_TRUE(torch::isfinite(v).all().item<bool>())
          << name << " " << dtype;
    }
  }
}

// Threads share one program and still match the reference
TEST(TorchScriptOpsImplTest, ThreadsShareProgram) {
  const std::vector<OpSpec> specs = {
      {OpKind::SearchSorted, 64, 16, 64, 1},
      {OpKind::IndexSelect, 256, 256, 1024, 1},
      {OpKind::LayerNorm, 512, 256, 4, 1},
      {OpKind::ToHalf, 512, 256, 4, 1}};
  const int nbuffers = 4;
  GraphSpec graph;
  graph.nops = 50;
  graph.rows = 4;
  graph.cols = 16;
  graph.dtypes = kDTypes;
  ChaseSpec chase;
  chase.len = 100;
  chase.lines = 4096;
  chase.width = 4;
  auto program = compileProgram(specs, nbuffers, graph, chase);
  std::vector<std::vector<std::vector<char>>> want(nbuffers);
  for (int b = 0; nbuffers > b; ++b) {
    for (size_t k = 0; specs.size() > k; ++k) {
      want[b].push_back(expectedOutput(specs, nbuffers, k, b));
    }
  }
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; 4 > t; ++t) {
    threads.emplace_back([&, t]() {
      auto frame = program->newFrame(100 + t);
      const int b = t % nbuffers;
      for (int i = 0; 20 > i; ++i) {
        frame->fire();
        frame->fireBuffer(b);
        for (size_t k = 0; specs.size() > k; ++k) {
          mismatches += !matches(specs[k].kind, frame->output(k), want[b][k]);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(mismatches.load(), 0);
}

} // namespace
} // namespace facebook::cea::chips::adsim::torchscript_ops
