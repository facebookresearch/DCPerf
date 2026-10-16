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

#include <ATen/CPUGeneratorImpl.h>
#include <torch/csrc/jit/codegen/fuser/interface.h> // @manual=//caffe2:torch-cpp-cpu
#include <torch/script.h>
#include <torch/torch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace facebook::cea::chips::adsim::torchscript_ops {

namespace {

using Inputs = std::vector<std::shared_ptr<OpInputs>>;

template <typename... Parts>
std::string cat(const Parts&... parts) {
  std::string s;
  (s += ... += parts);
  return s;
}

at::Tensor view(const void* p, at::IntArrayRef sizes, at::ScalarType t) {
  return torch::from_blob(
      const_cast<void*>(p), sizes, at::TensorOptions().dtype(t));
}

// The TorchScript function `ops`, writing op k's output to ws[k]
struct Ops {
  std::string source;
  std::vector<c10::IValue> args;
  c10::List<at::Tensor> ws;
  int64_t nix = 0;
};

Ops buildOps(const Inputs& inputs) {
  Ops ops;
  std::ostringstream sig;
  std::ostringstream body;
  sig << "def ops(ws: List[Tensor], ix: List[int]";
  for (size_t k = 0; inputs.size() > k; ++k) {
    const auto& in = *inputs[k];
    const auto& s = in.spec();
    const int64_t n = s.rows * s.cols;
    auto perBuffer = [&](const std::string& name,
                         const std::function<at::Tensor(int)>& make) {
      c10::List<at::Tensor> list;
      for (int b = 0; in.nbuffers() > b; ++b) {
        list.push_back(make(b));
      }
      sig << ", " << name << ": List[Tensor]";
      ops.args.emplace_back(list);
    };
    auto floats = [&](const std::string& name,
                      int64_t offset,
                      int64_t rows,
                      int64_t cols) {
      perBuffer(name, [&](int b) {
        return view(in.f32(b) + offset, {rows, cols}, at::kFloat);
      });
    };
    const std::string a = "a" + std::to_string(k) + "_";
    const std::string w = "ws[" + std::to_string(k) + "]";
    std::string stmt;
    at::Tensor out = at::empty({0});
    switch (s.kind) {
      case OpKind::Copy:
        floats(a + "x", 0, s.rows, s.cols);
        out = at::empty({s.rows, s.cols});
        stmt = cat(w, ".copy_(", a, "x[b])");
        break;
      case OpKind::Fill: {
        c10::List<double> values;
        for (int b = 0; in.nbuffers() > b; ++b) {
          values.push_back(in.f32(b)[0]);
        }
        sig << ", " << a << "v: List[float]";
        ops.args.emplace_back(values);
        out = at::empty({s.rows, s.cols});
        stmt = cat(w, ".fill_(", a, "v[b])");
        break;
      }
      case OpKind::Add:
        floats(a + "x", 0, s.rows, s.cols);
        floats(a + "y", n, s.rows, s.cols);
        stmt = cat(w, " = torch.add(", a, "x[b], ", a, "y[b], alpha=0.5)");
        break;
      case OpKind::LayerNorm:
        floats(a + "x", 0, s.rows, s.cols);
        perBuffer(a + "g", [&](int b) {
          return view(in.f32(b) + n, {s.cols}, at::kFloat);
        });
        perBuffer(a + "h", [&](int b) {
          return view(in.f32(b) + n + s.cols, {s.cols}, at::kFloat);
        });
        stmt =
            cat(w,
                " = torch.layer_norm(",
                a,
                "x[b], [",
                std::to_string(s.cols),
                "], ",
                a,
                "g[b], ",
                a,
                "h[b], 1e-5)");
        break;
      case OpKind::SearchSorted:
        floats(a + "x", 0, s.rows, s.cols);
        floats(a + "s", n, s.rows, s.aux);
        stmt = cat(w, " = torch.searchsorted(", a, "s[b], ", a, "x[b])");
        break;
      case OpKind::ClampRangeLengths:
        perBuffer(
            a + "r", [&](int b) { return view(in.i32(b), {n, 2}, at::kInt); });
        stmt =
            cat("t = ",
                a,
                "r[b].clone()\n        t.select(1, 1).clamp_max_(16)\n        ",
                w,
                " = t");
        break;
      case OpKind::CompleteCumsum:
        perBuffer(
            a + "l", [&](int b) { return view(in.i32(b), {n}, at::kInt); });
        // dtype=4 is torch.int64.
        stmt = cat(
            "t = torch.empty([",
            std::to_string(n + 1),
            "], dtype=4)\n        t.narrow(0, 0, 1).zero_()\n        torch.cumsum(",
            a,
            "l[b], 0, dtype=4, out=t.narrow(0, 1, ",
            std::to_string(n),
            "))\n        ",
            w,
            " = t");
        break;
      case OpKind::Diff:
        perBuffer(
            a + "x", [&](int b) { return view(in.f32(b), {n}, at::kFloat); });
        stmt = cat(w, " = torch.diff(", a, "x[b])");
        break;
      case OpKind::IndexSelect:
        floats(a + "t", 0, s.aux, s.cols);
        perBuffer(a + "i", [&](int b) {
          return view(in.index(b), {s.rows}, at::kLong);
        });
        stmt = cat(w, " = torch.index_select(", a, "t[b], 0, ", a, "i[b])");
        break;
      case OpKind::Cat: {
        c10::List<c10::List<at::Tensor>> parts;
        for (int b = 0; in.nbuffers() > b; ++b) {
          c10::List<at::Tensor> list;
          for (int64_t q = 0; s.aux > q; ++q) {
            list.push_back(
                view(in.f32(b) + q * n, {s.rows, s.cols}, at::kFloat));
          }
          parts.push_back(list);
        }
        sig << ", " << a << "p: List[List[Tensor]]";
        ops.args.emplace_back(parts);
        stmt = cat(w, " = torch.cat(", a, "p[b], 1)");
        break;
      }
      case OpKind::ToHalf:
        floats(a + "x", 0, s.rows, s.cols);
        stmt = cat(w, " = ", a, "x[b].half()");
        break;
    }
    ops.ws.push_back(out);
    body << "    for r in range(" << s.reps << "):\n"
         << "        b = ix[" << ops.nix << " + r]\n"
         << "        " << stmt << "\n";
    ops.nix += s.reps;
  }
  sig << ") -> int:\n";
  ops.source =
      sig.str() + body.str() + "    return " + std::to_string(ops.nix) + "\n";
  return ops;
}

// A dtype the graph runs in. `narrow` rounds a value through a narrower type.
struct DType {
  const char* name;
  at::ScalarType type;
  std::string narrow;
  bool lowp; // bfloat16 or float16
};

// Cast by ScalarType number
std::string castTo(at::ScalarType t) {
  return ".to(" + std::to_string(static_cast<int>(t)) + ")";
}

const DType& parseDType(const std::string& name) {
  static const DType kDTypes[] = {
      {"float32", at::kFloat, ".half().float()", false},
      {"float64", at::kDouble, ".half()" + castTo(at::kDouble), false},
      {"bfloat16", at::kBFloat16, ".half()" + castTo(at::kBFloat16), true},
      {"float16", at::kHalf, castTo(at::kBFloat16) + ".half()", true},
  };
  for (const auto& t : kDTypes) {
    if (name == t.name) {
      return t;
    }
  }
  throw std::invalid_argument(
      "TorchScriptOps graph dtype " + name +
      " is not float32, float64, bfloat16, or float16");
}

std::vector<const DType*> parseDTypes(const std::vector<std::string>& names) {
  std::vector<const DType*> types;
  for (const auto& name : names) {
    types.push_back(&parseDType(name));
    if (std::count(names.begin(), names.end(), name) > 1) {
      throw std::invalid_argument(
          "TorchScriptOps graph dtype " + name + " is listed twice");
    }
  }
  if (types.empty()) {
    throw std::invalid_argument("TorchScriptOps graph needs a dtype");
  }
  return types;
}

// An op on `nin` rows x cols tensors. `expr` placeholders: $0-$2 inputs, $R/$C
// rows/cols, $H/$Q half/quarter cols, $B clamp, $T cast, $N narrowing.
struct OpTemplate {
  const char* name;
  int nin;
  const char* expr;
  bool lowp = true;
};

const std::vector<OpTemplate>& opTemplates() {
  static const std::vector<OpTemplate> kOps = {
      {"relu", 1, "torch.relu($0)"},
      {"sigmoid", 1, "torch.sigmoid($0)"},
      {"tanh", 1, "torch.tanh($0)"},
      {"abs", 1, "torch.abs($0)"},
      {"clamp", 1, "torch.clamp($0, -1.0, 1.0)"},
      {"add", 2, "torch.add($0, $1)$B"},
      {"mul", 2, "torch.mul($0, $1)$B"},
      {"add_alpha", 2, "torch.add($0, $1, alpha=0.25)$B"},
      {"maximum", 2, "torch.maximum($0, $1)"},
      {"where", 2, "torch.where($0 > $1, $0, $1)"},
      {"addcmul", 3, "torch.addcmul($0, $1, $2, value=0.5)$B"},
      {"masked_fill", 1, "$0.masked_fill($0 < 0.5, 0.0)"},
      {"center", 1, "($0 - $0.mean(1, keepdim=True))$B"},
      {"gate", 1, "$0 * torch.sigmoid($0.sum(1, keepdim=True))"},
      {"softmax", 1, "torch.softmax($0, 1)"},
      {"layer_norm", 1, "torch.layer_norm($0, [$C])"},
      {"cat_narrow", 2, "torch.cat([$0, $1], 1).narrow(1, $H, $C)"},
      {"roll", 1, "torch.cat([$0[:, $Q:], $0[:, :$Q]], 1)"},
      {"split_swap", 1, "torch.cat(torch.split($0, $H, 1)[::-1], 1)"},
      {"index_select", 1, "torch.index_select($0, 0, idx)"},
      {"to_half", 1, "$0$N"},
      {"transpose", 1, "$0.t().contiguous().t()"},
      // Pointwise.
      {"exp", 1, "torch.exp($0)$B"},
      {"exp2", 1, "torch.exp2($0)$B"},
      {"expm1", 1, "torch.expm1($0)$B"},
      {"log1p", 1, "torch.log1p($0.abs())"},
      {"log2", 1, "torch.log2($0.abs() + 1.0)"},
      {"log10", 1, "torch.log10($0.abs() + 1.0)"},
      {"sqrt", 1, "torch.sqrt($0.abs())"},
      {"rsqrt", 1, "torch.rsqrt($0.abs() + 1.0)"},
      {"reciprocal", 1, "torch.reciprocal($0.abs() + 1.0)"},
      {"square", 1, "torch.square($0)$B"},
      {"pow", 1, "torch.pow($0.abs(), 1.5)$B"},
      {"neg", 1, "torch.neg($0)"},
      {"sign", 1, "torch.sign($0)"},
      {"floor", 1, "torch.floor($0)"},
      {"ceil", 1, "torch.ceil($0)"},
      {"round", 1, "torch.round($0)"},
      {"trunc", 1, "torch.trunc($0)"},
      {"frac", 1, "torch.frac($0)"},
      {"sin", 1, "torch.sin($0)"},
      {"cos", 1, "torch.cos($0)"},
      {"tan", 1, "torch.tan($0.clamp(-1.0, 1.0))"},
      {"asin", 1, "torch.asin($0.clamp(-1.0, 1.0))"},
      {"acos", 1, "torch.acos($0.clamp(-1.0, 1.0))"},
      {"atan", 1, "torch.atan($0)"},
      {"sinh", 1, "torch.sinh($0)$B"},
      {"cosh", 1, "torch.cosh($0)$B"},
      {"asinh", 1, "torch.asinh($0)"},
      {"acosh", 1, "torch.acosh($0.abs() + 1.0)"},
      {"atanh", 1, "torch.atanh($0.clamp(-0.9, 0.9))"},
      {"erf", 1, "torch.erf($0)"},
      {"erfc", 1, "torch.erfc($0)"},
      {"erfinv", 1, "torch.erfinv($0.clamp(-0.9, 0.9))"},
      {"lgamma", 1, "torch.lgamma($0.abs() + 1.0)$B"},
      {"digamma", 1, "torch.digamma($0.abs() + 1.0)"},
      {"i0", 1, "torch.special_i0($0)$B"},
      {"sinc", 1, "torch.sinc($0)"},
      {"deg2rad", 1, "torch.deg2rad($0)"},
      {"nan_to_num", 1, "torch.nan_to_num($0)"},
      {"isnan_where", 2, "torch.where(torch.isnan($0), $1, $0)"},
      {"logit", 1, "torch.logit(torch.sigmoid($0), 0.01)"},
      {"frexp", 1, "torch.frexp($0)[0]"},
      {"ldexp", 1, "torch.ldexp($0, torch.ones_like($0))$B"},
      // Activations.
      {"gelu", 1, "torch.gelu($0)"},
      {"silu", 1, "torch.silu($0)"},
      {"mish", 1, "torch.mish($0)"},
      {"elu", 1, "torch.elu($0)"},
      {"selu", 1, "torch.selu($0)"},
      {"celu", 1, "torch.celu($0)"},
      {"leaky_relu", 1, "torch.leaky_relu($0)"},
      {"rrelu", 1, "torch.rrelu($0)"},
      {"relu6", 1, "torch.relu6($0)"},
      {"hardtanh", 1, "torch.hardtanh($0)"},
      {"hardsigmoid", 1, "torch.hardsigmoid($0)"},
      {"hardswish", 1, "torch.hardswish($0)"},
      {"hardshrink", 1, "torch.hardshrink($0)"},
      {"softshrink", 1, "torch.softshrink($0)"},
      {"softplus", 1, "torch.softplus($0)"},
      {"log_sigmoid", 1, "torch.log_sigmoid($0)"},
      {"threshold", 1, "torch.threshold($0, 0.1, 0.0)"},
      {"clamp_min", 1, "torch.clamp_min($0, 0.1)"},
      {"clamp_max", 1, "torch.clamp_max($0, 0.1)"},
      // Binary and ternary.
      {"sub", 2, "torch.sub($0, $1)"},
      {"div", 2, "torch.div($0, $1.abs() + 1.0)"},
      {"minimum", 2, "torch.minimum($0, $1)"},
      {"fmax", 2, "torch.fmax($0, $1)"},
      {"fmin", 2, "torch.fmin($0, $1)"},
      {"fmod", 2, "torch.fmod($0, $1.abs() + 1.0)"},
      {"remainder", 2, "torch.remainder($0, $1.abs() + 1.0)"},
      {"atan2", 2, "torch.atan2($0, $1)"},
      {"hypot", 2, "torch.hypot($0, $1)$B"},
      {"copysign", 2, "torch.copysign($0, $1)"},
      {"nextafter", 2, "torch.nextafter($0, $1)"},
      {"heaviside", 2, "torch.heaviside($0, $1)"},
      {"xlogy", 2, "torch.xlogy($0.abs(), $1.abs() + 1.0)$B"},
      {"lerp", 2, "torch.lerp($0, $1, 0.3)"},
      {"logaddexp", 2, "torch.logaddexp($0, $1)$B"},
      {"logical_and", 2, "torch.logical_and($0 > 0.0, $1 > 0.0)$T"},
      {"addcdiv", 3, "torch.addcdiv($0, $1, $2.abs() + 1.0, value=0.5)"},
      {"stack", 2, "torch.stack([$0, $1]).sum(0)$B"},
      {"polar", 2, "torch.polar($0.abs(), $1).real", false},
      // Reductions along rows, broadcast back.
      {"amax", 1, "($0 * torch.amax($0, [1], True))$B"},
      {"amin", 1, "($0 - torch.amin($0, [1], True))$B"},
      {"aminmax", 1, "(torch.aminmax($0, dim=1, keepdim=True)[0] + $0)$B"},
      {"logsumexp", 1, "($0 - torch.logsumexp($0, [1], True))$B"},
      {"var", 1, "($0 - torch.var($0, [1], True, True))$B"},
      {"std", 1, "($0 - torch.std($0, [1], True, True))$B"},
      {"nanmean", 1, "(torch.nanmean($0, [1], True) + $0)$B"},
      {"norm", 1, "$0 / (torch.norm($0, 2, [1], True) + 1.0)"},
      {"prod", 1, "$0 * torch.prod($0.clamp(-1.0, 1.0), 1, True)"},
      {"median", 1, "(torch.median($0, 1, True)[0] + $0)$B"},
      {"kthvalue", 1, "(torch.kthvalue($0, 3, 1, True)[0] * $0)$B"},
      {"argmax_gather",
       1,
       "($0 - torch.gather($0, 1, torch.argmax($0, 1, True)))$B"},
      {"dist", 2, "(torch.dist($0, $1) / $0.numel() ** 0.5 + $0)$B"},
      {"renorm", 1, "torch.renorm($0, 2.0, 0, 1.0)"},
      {"group_norm", 1, "torch.group_norm($0, 4)"},
      {"rms_norm", 1, "torch.rms_norm($0, [$C])"},
      {"log_softmax", 1, "torch.log_softmax($0, 1)$B"},
      // Scans, sorts, and searches.
      {"cumsum", 1, "torch.cumsum($0, 1)$B"},
      {"cummax", 1, "torch.cummax($0, 1)[0]"},
      {"cummin", 1, "torch.cummin($0, 1)[0]"},
      {"logcumsumexp", 1, "torch.logcumsumexp($0, 1)$B"},
      {"sort", 1, "torch.sort($0, 1)[0]"},
      {"msort", 1, "torch.msort($0)"},
      {"bucketize", 1, "torch.bucketize($0.contiguous(), bnd)$T"},
      {"searchsorted", 1, "torch.searchsorted(bnd, $0.contiguous())$T"},
      {"diff", 1, "torch.diff($0, 1, 1, None, $0[:, :1])"},
      // Layout and indexing.
      {"flip", 1, "torch.flip($0, [1])"},
      {"roll_op", 1, "torch.roll($0, 3, 1)"},
      {"tril", 1, "torch.tril($0)"},
      {"triu", 1, "torch.triu($0)"},
      {"pad", 1, "torch.constant_pad_nd($0, [1, 1]).narrow(1, 1, $C)"},
      {"index_add", 2, "torch.index_add($0, 0, idx, $1)$B"},
      {"index_fill", 1, "torch.index_fill($0, 0, idx, 0.5)"},
      {"cross",
       2,
       "torch.cross($0[:, :3], $1[:, :3], 1).repeat(1, $C).narrow(1, 0, $C)$B"},
      {"outer", 2, "torch.outer($0[:, 0], $1[0])$B"},
      {"kron", 2, "torch.kron($0[:1], $1[:, :1])$B"},
      // Matrix products with w.
      {"matmul", 1, "torch.matmul($0, w)$B"},
      {"addmm", 2, "torch.addmm($1, $0, w, beta=0.5)$B"},
      {"bmm", 1, "torch.bmm($0.unsqueeze(0), w.unsqueeze(0)).squeeze(0)$B"},
      {"einsum", 1, "torch.einsum('ij,jk->ik', [$0, w])$B"},
      {"trace", 1, "(torch.trace(w) + $0)$B"},
  };
  return kOps;
}

// `op.expr` with its placeholders filled in.
std::string emit(
    const OpTemplate& op,
    const std::vector<std::string>& in,
    const GraphSpec& g,
    const DType& dtype) {
  std::string s;
  for (const char* p = op.expr; *p; ++p) {
    if ('$' != *p) {
      s += *p;
      continue;
    }
    switch (*++p) {
      case '0':
      case '1':
      case '2':
        s += in.at(*p - '0');
        break;
      case 'R':
        s += std::to_string(g.rows);
        break;
      case 'C':
        s += std::to_string(g.cols);
        break;
      case 'H':
        s += std::to_string(g.cols / 2);
        break;
      case 'Q':
        s += std::to_string(g.cols / 4);
        break;
      case 'B':
        s += ".clamp_(-4.0, 4.0)";
        break;
      case 'T':
        s += castTo(dtype.type);
        break;
      case 'N':
        s += dtype.narrow;
        break;
      default:
        throw std::logic_error(
            std::string("TorchScriptOps template ") + op.name +
            " has a bad placeholder");
    }
  }
  return s;
}

// The templates in `names`, or all of them, that run in `dtype`
std::vector<const OpTemplate*> selectTemplates(
    const std::vector<std::string>& names,
    const DType& dtype) {
  std::set<std::string> wanted(names.begin(), names.end());
  if (wanted.size() != names.size()) {
    throw std::invalid_argument("TorchScriptOps graph lists a template twice");
  }
  std::vector<const OpTemplate*> ops;
  for (const auto& op : opTemplates()) {
    const bool listed = names.empty() || wanted.erase(op.name);
    if (listed && (op.lowp || !dtype.lowp)) {
      ops.push_back(&op);
    }
  }
  if (!wanted.empty()) {
    throw std::invalid_argument(
        "TorchScriptOps graph template " + *wanted.begin() + " is unknown");
  }
  if (ops.empty()) {
    throw std::invalid_argument(
        std::string("TorchScriptOps graph has no templates for ") + dtype.name);
  }
  return ops;
}

// The generated graph in one dtype
struct GraphBody {
  std::string text;
  std::vector<std::string> unread;
};

GraphBody buildGraph(const GraphSpec& g, const DType& dtype) {
  if (0 >= g.nops || 0 >= g.ninputs || 0 >= g.nregs || 0 >= g.rows ||
      4 > g.cols || 0 != g.cols % 4) {
    throw std::invalid_argument(
        "TorchScriptOps graph needs positive nops, ninputs, nregs, and rows, "
        "and cols a positive multiple of 4");
  }
  std::mt19937_64 rng(g.seed);
  const auto ops = selectTemplates(g.templates, dtype);
  std::vector<double> weights;
  for (size_t i = 0; ops.size() > i; ++i) {
    weights.push_back(1.0 / std::pow(i + 1.0, g.zipf_s));
  }
  std::shuffle(weights.begin(), weights.end(), rng);
  std::discrete_distribution<int> pickOp(weights.begin(), weights.end());

  GraphBody body;
  std::ostringstream src;
  std::vector<std::string> live;
  auto& unread = body.unread;
  for (int i = 0; g.ninputs > i; ++i) {
    live.push_back("v" + std::to_string(i));
    src << "    " << live.back() << " = x[" << i << "]\n";
  }
  src << "    w = x[" << g.ninputs << "]\n"
      << "    bnd = x[" << g.ninputs + 1 << "]\n";
  auto pick = [&]() {
    if (!unread.empty() && 0 == rng() % 2) {
      std::string v = unread.front();
      unread.erase(unread.begin());
      return v;
    }
    std::string v = live[rng() % live.size()];
    unread.erase(std::remove(unread.begin(), unread.end(), v), unread.end());
    return v;
  };
  // Redraw duplicate expressions
  std::set<std::string> seen;
  std::vector<const OpTemplate*> pass;
  for (int k = 0; g.nops > k; ++k) {
    const OpTemplate* op = ops.at(0);
    if (g.cover) {
      if (pass.empty()) {
        pass = ops;
        std::shuffle(pass.begin(), pass.end(), rng);
      }
      if (!pass.empty()) {
        op = pass.back();
        pass.pop_back();
      }
    }
    std::string expr;
    const auto saved = unread;
    for (int tries = 0; expr.empty() || !seen.insert(expr).second; ++tries) {
      if (10000 == tries) {
        throw std::invalid_argument(
            "TorchScriptOps graph: too few live values for distinct ops");
      }
      unread = saved;
      if (!g.cover) {
        op = ops[pickOp(rng)];
      }
      std::vector<std::string> in;
      for (int i = 0; op->nin > i; ++i) {
        in.push_back(pick());
      }
      expr = emit(*op, in, g, dtype);
    }
    const std::string out = "v" + std::to_string(g.ninputs + k);
    src << "    " << out << " = " << expr << "\n";
    live.push_back(out);
    unread.push_back(out);
    while (static_cast<int>(live.size()) > g.nregs) {
      live.erase(live.begin());
    }
  }
  body.text = src.str();
  return body;
}

std::string graphFunction(
    const GraphBody& body,
    const std::vector<std::string>& ret,
    const DType& dtype) {
  std::string f = std::string("def graph_") + dtype.name +
      "(x: List[Tensor], idx: Tensor) -> List[Tensor]:\n" + body.text +
      "    return [";
  for (size_t i = 0; ret.size() > i; ++i) {
    f += (i ? ", " : "") + ret[i];
  }
  return f + "]\n";
}

// Pointer chase: each hop index_selects the line its previous result names
constexpr const char* kChaseSource = R"(
def chase(t: Tensor, j: Tensor, n: int) -> Tensor:
    for i in range(n):
        j = torch.index_select(t, 0, j)
    return j
)";

void configureTorchOnce() {
  [[maybe_unused]] static const bool once = [] {
    // Run every kernel on the caller's thread
    at::set_num_threads(1);
    at::set_num_interop_threads(1);
    // Run every op as an ATen kernel, without CPU fusion
    torch::jit::overrideCanFuseOnCPU(false);
    return true;
  }();
}

class ProgramImpl : public Program,
                    public std::enable_shared_from_this<ProgramImpl> {
 public:
  ProgramImpl(
      const std::vector<OpSpec>& specs,
      int nbuffers,
      const GraphSpec& graph,
      const ChaseSpec& chase)
      : graph_(graph), chase_(chase) {
    if (specs.empty() && 0 >= graph.nops && 0 >= chase.len) {
      throw std::invalid_argument(
          "TorchScriptOps needs ops, a graph, or a chase");
    }
    for (size_t i = 0; specs.size() > i; ++i) {
      inputs_.push_back(
          std::make_shared<OpInputs>(specs[i], nbuffers, /*seed=*/i + 1));
      nops_ += specs[i].reps;
      inputBytes_ += inputs_.back()->inputElems() * sizeof(float) *
          inputs_.back()->nbuffers();
    }
    if (!inputs_.empty()) {
      source_ = buildOps(inputs_).source;
    }
    if (0 < graph.nops) {
      if (1 > graph.nruns) {
        throw std::invalid_argument(
            "TorchScriptOps graph needs positive nruns");
      }
      dtypes_ = parseDTypes(graph.dtypes);
      for (const DType* t : dtypes_) {
        const GraphBody body = buildGraph(graph, *t);
        source_ += graphFunction(body, body.unread, *t);
      }
      auto gen = at::make_generator<at::CPUGeneratorImpl>(graph.seed);
      std::vector<at::Tensor> inputs;
      for (int i = 0; graph.ninputs > i; ++i) {
        inputs.push_back(at::rand({graph.rows, graph.cols}, gen));
      }
      inputs.push_back((at::rand({graph.cols, graph.cols}, gen) - 0.5) * 0.25);
      inputs.push_back(at::linspace(-2.0, 2.0, 9));
      idx_ = at::randint(graph.rows, {graph.rows}, gen, at::kLong);
      for (const DType* t : dtypes_) {
        graphInputs_.emplace_back();
        for (const auto& x : inputs) {
          graphInputs_.back().push_back(x.to(t->type));
        }
      }
    }
    if (0 < chase.len) {
      if (1 > chase.lines || 1 > chase.width) {
        throw std::invalid_argument(
            "TorchScriptOps chase needs positive lines and width");
      }
      source_ += kChaseSource;
      // Sattolo's shuffle, which leaves one cycle through every line.
      std::vector<int64_t> order(chase.lines);
      std::iota(order.begin(), order.end(), 0);
      std::mt19937_64 rng(chase.seed);
      for (int64_t i = chase.lines - 1; 0 < i; --i) {
        std::swap(order[i], order[rng() % i]);
      }
      chaseTable_ = at::zeros({chase.lines * kLineInts}, at::kLong);
      auto* next = chaseTable_.data_ptr<int64_t>();
      for (int64_t i = 0; chase.lines > i; ++i) {
        next[order[i] * kLineInts] = order[(i + 1) % chase.lines] * kLineInts;
      }
    }
    cu_ = torch::jit::compile(source_);
  }

  std::unique_ptr<Frame> newFrame(uint64_t seed) const override;

  std::string describe() const override {
    std::ostringstream s;
    if (0 < nops_) {
      s << nops_ << " op calls on " << inputBytes_ << "B inputs";
    }
    if (0 < graph_.nops) {
      s << (0 < nops_ ? ", " : "") << "graph of " << graph_.nops << " ops x "
        << graph_.nruns << " runs in";
      for (const DType* t : dtypes_) {
        s << " " << t->name;
      }
    }
    if (0 < chase_.len) {
      s << (0 < nops_ || 0 < graph_.nops ? ", " : "") << "chase of "
        << chase_.len << " hops x " << chase_.width << " walks over "
        << chase_.lines << " lines";
    }
    return s.str();
  }

  const std::string& source() const override {
    return source_;
  }

 private:
  friend class FrameImpl;

  static constexpr int64_t kLineInts = 64 / sizeof(int64_t);

  GraphSpec graph_;
  ChaseSpec chase_;
  std::vector<const DType*> dtypes_;
  Inputs inputs_;
  int64_t nops_ = 0;
  int64_t inputBytes_ = 0;
  std::string source_;
  std::shared_ptr<torch::jit::CompilationUnit> cu_;
  std::vector<std::vector<at::Tensor>> graphInputs_; // Per dtype.
  at::Tensor idx_;
  at::Tensor chaseTable_;
};

class FrameImpl : public Frame {
 public:
  FrameImpl(std::shared_ptr<const ProgramImpl> program, uint64_t seed)
      : program_(std::move(program)), rng_(seed) {
    c10::InferenceMode guard;
    if (!program_->inputs_.empty()) {
      Ops ops = buildOps(program_->inputs_);
      args_ = std::move(ops.args);
      ws_ = ops.ws;
      ix_ = c10::List<int64_t>(std::vector<int64_t>(ops.nix, 0));
      opsFn_ = &program_->cu_->get_function("ops");
    }
    if (0 < program_->graph_.nops) {
      // Per-thread copies of the inputs
      for (size_t d = 0; program_->dtypes_.size() > d; ++d) {
        c10::List<at::Tensor> inputs;
        for (const auto& t : program_->graphInputs_[d]) {
          inputs.push_back(t.clone());
        }
        graphInputs_.emplace_back(inputs);
        graphFns_.push_back(&program_->cu_->get_function(
            std::string("graph_") + program_->dtypes_[d]->name));
      }
      next_ = rng_() % graphFns_.size();
      idx_ = program_->idx_.clone();
    }
    if (0 < program_->chase_.len) {
      // Per-thread view of the shared table
      chaseTable_ = program_->chaseTable_.view({-1});
      std::vector<int64_t> starts;
      for (int i = 0; program_->chase_.width > i; ++i) {
        starts.push_back(
            static_cast<int64_t>(rng_() % program_->chase_.lines) *
            ProgramImpl::kLineInts);
      }
      chaseAt_ = at::tensor(starts, at::kLong);
      chaseFn_ = &program_->cu_->get_function("chase");
    }
  }

  int64_t fire() override {
    c10::InferenceMode guard;
    int64_t n = 0;
    if (opsFn_) {
      const int nbuffers = program_->inputs_[0]->nbuffers();
      for (size_t i = 0; ix_.size() > i; ++i) {
        ix_.set(i, static_cast<int64_t>(rng_() % nbuffers));
      }
      n += runOps();
    }
    for (int i = 0; !graphFns_.empty() && program_->graph_.nruns > i; ++i) {
      n += static_cast<int64_t>(
          (*graphFns_.at(next_))({graphInputs_.at(next_), idx_})
              .toList()
              .size());
      next_ = (next_ + 1) % graphFns_.size();
    }
    if (chaseFn_) {
      chaseAt_ =
          (*chaseFn_)({chaseTable_, chaseAt_, program_->chase_.len}).toTensor();
      n += program_->chase_.len;
    }
    return n;
  }

  int64_t fireBuffer(int b) override {
    c10::InferenceMode guard;
    for (size_t i = 0; ix_.size() > i; ++i) {
      ix_.set(i, b);
    }
    return opsFn_ ? runOps() : 0;
  }

  std::vector<char> output(size_t k) const override {
    const at::Tensor t = ws_.get(k).contiguous();
    std::vector<char> bytes(t.nbytes());
    std::memcpy(bytes.data(), t.data_ptr(), bytes.size());
    return bytes;
  }

 private:
  int64_t runOps() {
    std::vector<c10::IValue> stack = {ws_, ix_};
    stack.insert(stack.end(), args_.begin(), args_.end());
    return (*opsFn_)(std::move(stack)).toInt();
  }

  std::shared_ptr<const ProgramImpl> program_;
  std::mt19937_64 rng_;
  torch::jit::Function* opsFn_ = nullptr;
  std::vector<c10::IValue> args_;
  c10::List<at::Tensor> ws_;
  c10::List<int64_t> ix_;
  std::vector<torch::jit::Function*> graphFns_; // Per dtype.
  std::vector<c10::IValue> graphInputs_;
  // Dtype of the next graph run, starting at random
  size_t next_ = 0;
  at::Tensor idx_;
  torch::jit::Function* chaseFn_ = nullptr;
  at::Tensor chaseTable_;
  at::Tensor chaseAt_; // Current line of each walk.
};

std::unique_ptr<Frame> ProgramImpl::newFrame(uint64_t seed) const {
  return std::make_unique<FrameImpl>(shared_from_this(), seed);
}

} // namespace

std::shared_ptr<Program> compileProgram(
    const std::vector<OpSpec>& specs,
    int nbuffers,
    const GraphSpec& graph,
    const ChaseSpec& chase) {
  configureTorchOnce();
  c10::InferenceMode guard;
  auto program = std::make_shared<ProgramImpl>(specs, nbuffers, graph, chase);
  auto frame = program->newFrame(/*seed=*/0);
  for (size_t i = 0; 4 * std::max<size_t>(1, graph.dtypes.size()) > i; ++i) {
    frame->fire();
  }
  return program;
}

std::vector<std::string> templateNames() {
  std::vector<std::string> names;
  for (const auto& op : opTemplates()) {
    names.emplace_back(op.name);
  }
  return names;
}

std::string graphSource(const GraphSpec& graph, bool returnAll) {
  std::vector<std::string> all;
  for (int i = 0; graph.ninputs + graph.nops > i; ++i) {
    all.push_back("v" + std::to_string(i));
  }
  std::string source;
  for (const DType* t : parseDTypes(graph.dtypes)) {
    const GraphBody body = buildGraph(graph, *t);
    source += graphFunction(body, returnAll ? all : body.unread, *t);
  }
  return source;
}

} // namespace facebook::cea::chips::adsim::torchscript_ops
