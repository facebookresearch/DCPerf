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

// CPU tensor operators that an inference server runs on the host around a
// model whose dense layers run on a GPU: copies and fills, elementwise add,
// LayerNorm, searchsorted, and sparse-feature index helpers (range length
// clamping, lengths to offsets, diff, gather, cat, fp32 to fp16). Each follows
// the loop structure of the matching ATen CPU kernel. The code has no AdSim
// dependencies so it can be tested standalone.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#define TENSOR_OPS_TARGET __attribute__((target("avx2,fma,f16c")))
#elif defined(__aarch64__)
#include <arm_neon.h>
#define TENSOR_OPS_TARGET
#else
#error "TensorOps supports x86_64 and aarch64"
#endif

// Keep explicit loops from being rewritten into memcpy/memset calls, since
// ATen's copy and fill kernels are vectorized loops.
#if defined(__clang__)
#define TENSOR_OPS_NO_BUILTIN __attribute__((no_builtin("memcpy", "memset")))
#else
#define TENSOR_OPS_NO_BUILTIN
#endif

namespace facebook::cea::chips::adsim::tensor_ops {

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
  throw std::invalid_argument("unknown TensorOps op: " + name);
}

TENSOR_OPS_TARGET TENSOR_OPS_NO_BUILTIN inline void
copyKernel(float* __restrict dst, const float* __restrict src, int64_t n) {
  for (int64_t i = 0; n > i; ++i) {
    dst[i] = src[i];
  }
}

TENSOR_OPS_TARGET TENSOR_OPS_NO_BUILTIN inline void
fillKernel(float* __restrict dst, float value, int64_t n) {
  for (int64_t i = 0; n > i; ++i) {
    dst[i] = value;
  }
}

TENSOR_OPS_TARGET inline void addKernel(
    float* __restrict out,
    const float* __restrict a,
    const float* __restrict b,
    float alpha,
    int64_t n) {
  for (int64_t i = 0; n > i; ++i) {
    out[i] = a[i] + alpha * b[i];
  }
}

// Computes each row's moments and then applies the affine normalization, the
// two passes of ATen's LayerNormKernelImplInternal.
TENSOR_OPS_TARGET inline void layerNormKernel(
    float* __restrict out,
    const float* __restrict in,
    const float* __restrict gamma,
    const float* __restrict beta,
    int64_t rows,
    int64_t cols,
    float eps) {
  for (int64_t r = 0; rows > r; ++r) {
    const float* x = in + r * cols;
    float* y = out + r * cols;
    float sum = 0.f;
    float sq = 0.f;
    int64_t c = 0;
#if defined(__x86_64__)
    // ATen reduces the moments with explicit vectors (RowwiseMomentsImpl); the
    // compiler will not vectorize a float sum on its own without
    // reassociation.
    __m256 vsum = _mm256_setzero_ps();
    __m256 vsq = _mm256_setzero_ps();
    for (; cols >= c + 8; c += 8) {
      __m256 v = _mm256_loadu_ps(x + c);
      vsum = _mm256_add_ps(vsum, v);
      vsq = _mm256_fmadd_ps(v, v, vsq);
    }
    alignas(32) float lanes[16];
    _mm256_store_ps(lanes, vsum);
    _mm256_store_ps(lanes + 8, vsq);
    for (int l = 0; 8 > l; ++l) {
      sum += lanes[l];
      sq += lanes[8 + l];
    }
#elif defined(__aarch64__)
    // The same 8-wide reduction as the AVX2 path, as two 4-wide NEON halves.
    float32x4_t vsum0 = vdupq_n_f32(0.f);
    float32x4_t vsum1 = vdupq_n_f32(0.f);
    float32x4_t vsq0 = vdupq_n_f32(0.f);
    float32x4_t vsq1 = vdupq_n_f32(0.f);
    for (; cols >= c + 8; c += 8) {
      float32x4_t v0 = vld1q_f32(x + c);
      float32x4_t v1 = vld1q_f32(x + c + 4);
      vsum0 = vaddq_f32(vsum0, v0);
      vsum1 = vaddq_f32(vsum1, v1);
      vsq0 = vfmaq_f32(vsq0, v0, v0);
      vsq1 = vfmaq_f32(vsq1, v1, v1);
    }
    sum += vaddvq_f32(vaddq_f32(vsum0, vsum1));
    sq += vaddvq_f32(vaddq_f32(vsq0, vsq1));
#endif
    for (; cols > c; ++c) {
      sum += x[c];
      sq += x[c] * x[c];
    }
    float mean = sum / cols;
    float rstd = 1.f / std::sqrt(std::max(sq / cols - mean * mean, 0.f) + eps);
    for (c = 0; cols > c; ++c) {
      y[c] = (x[c] - mean) * rstd * gamma[c] + beta[c];
    }
  }
}

// Each row of `values` is searched in the matching row of `boundaries`, like
// ATen's searchsorted_cpu_contiguous with right=false.
template <typename T>
inline void searchSortedKernel(
    int64_t* __restrict out,
    const T* __restrict boundaries,
    const T* __restrict values,
    int64_t rows,
    int64_t nboundaries,
    int64_t nvalues) {
  for (int64_t r = 0; rows > r; ++r) {
    const T* b = boundaries + r * nboundaries;
    const T* v = values + r * nvalues;
    int64_t* o = out + r * nvalues;
    for (int64_t i = 0; nvalues > i; ++i) {
      o[i] = std::lower_bound(b, b + nboundaries, v[i]) - b;
    }
  }
}

// `ranges` holds (offset, length) pairs; lengths are clamped to `max_length`.
inline void clampRangeLengthsKernel(
    int32_t* __restrict out,
    const int32_t* __restrict ranges,
    int64_t nranges,
    int32_t max_length) {
  for (int64_t i = 0; nranges > i; ++i) {
    out[2 * i] = ranges[2 * i];
    out[2 * i + 1] = std::min(ranges[2 * i + 1], max_length);
  }
}

// Writes the n + 1 offsets of n lengths, starting with 0.
inline void completeCumsumKernel(
    int64_t* __restrict out,
    const int32_t* __restrict lengths,
    int64_t n) {
  int64_t acc = 0;
  out[0] = 0;
  for (int64_t i = 0; n > i; ++i) {
    acc += lengths[i];
    out[i + 1] = acc;
  }
}

TENSOR_OPS_TARGET inline void
diffKernel(float* __restrict out, const float* __restrict in, int64_t n) {
  for (int64_t i = 0; n - 1 > i; ++i) {
    out[i] = in[i + 1] - in[i];
  }
}

TENSOR_OPS_TARGET TENSOR_OPS_NO_BUILTIN inline void indexSelectKernel(
    float* __restrict out,
    const float* __restrict table,
    const int64_t* __restrict index,
    int64_t nindex,
    int64_t cols) {
  for (int64_t i = 0; nindex > i; ++i) {
    const float* src = table + index[i] * cols;
    float* dst = out + i * cols;
    for (int64_t c = 0; cols > c; ++c) {
      dst[c] = src[c];
    }
  }
}

// Concatenates `nparts` equal row blocks of `in` along the column dimension,
// like ATen's cat_serial_kernel on dim 1.
TENSOR_OPS_TARGET TENSOR_OPS_NO_BUILTIN inline void catKernel(
    float* __restrict out,
    const float* __restrict in,
    int64_t rows,
    int64_t cols,
    int64_t nparts) {
  for (int64_t r = 0; rows > r; ++r) {
    float* dst = out + r * cols * nparts;
    for (int64_t p = 0; nparts > p; ++p) {
      const float* src = in + (p * rows + r) * cols;
      for (int64_t c = 0; cols > c; ++c) {
        dst[p * cols + c] = src[c];
      }
    }
  }
}

// Converts to IEEE fp16 bit patterns with round-to-nearest-even.
TENSOR_OPS_TARGET inline void
toHalfKernel(uint16_t* __restrict out, const float* __restrict in, int64_t n) {
  int64_t i = 0;
#if defined(__x86_64__)
  for (; n - 8 >= i; i += 8) {
    __m128i h =
        _mm256_cvtps_ph(_mm256_loadu_ps(in + i), _MM_FROUND_TO_NEAREST_INT);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), h);
  }
  for (; n > i; ++i) {
    out[i] = _cvtss_sh(in[i], _MM_FROUND_TO_NEAREST_INT);
  }
#elif defined(__aarch64__)
  for (; n - 8 >= i; i += 8) {
    float16x8_t h = vcombine_f16(
        vcvt_f16_f32(vld1q_f32(in + i)), vcvt_f16_f32(vld1q_f32(in + i + 4)));
    vst1q_u16(out + i, vreinterpretq_u16_f16(h));
  }
  for (; n > i; ++i) {
    out[i] = vget_lane_u16(
        vreinterpret_u16_f16(vcvt_f16_f32(vdupq_n_f32(in[i]))), 0);
  }
#endif
}

/* Shape of one op invocation: `rows` x `cols` elements.
 *
 * searchsorted uses `aux` boundaries per row, cat concatenates `aux` parts,
 * and index_select gathers `rows` rows out of a table of `aux` rows.
 */
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
        "TensorOps rows, cols and aux must be positive and reps non-negative");
  }
  if (OpKind::Diff == spec.kind && 2 > spec.rows * spec.cols) {
    throw std::invalid_argument("TensorOps diff needs at least 2 elements");
  }
}

/* Read-only inputs of one op, replicated `nbuffers` times so successive
 * invocations touch different memory. */
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
        return 1; // Integer inputs live in i32_.
      case OpKind::Copy:
      case OpKind::Fill:
      case OpKind::Diff:
      case OpKind::ToHalf:
        break;
    }
    return n;
  }
  // Bytes of output workspace one invocation needs.
  int64_t outputBytes() const {
    const int64_t n = spec_.rows * spec_.cols;
    switch (spec_.kind) {
      case OpKind::SearchSorted:
        return n * sizeof(int64_t);
      case OpKind::CompleteCumsum:
        return (n + 1) * sizeof(int64_t);
      case OpKind::ClampRangeLengths:
        return 2 * n * sizeof(int32_t);
      case OpKind::Cat:
        return n * spec_.aux * sizeof(float);
      case OpKind::Copy:
      case OpKind::Fill:
      case OpKind::Add:
      case OpKind::LayerNorm:
      case OpKind::Diff:
      case OpKind::IndexSelect:
      case OpKind::ToHalf:
        break;
    }
    return n * sizeof(float);
  }

 private:
  OpSpec spec_;
  int nbuffers_;
  int64_t stride_ = 0;
  std::vector<float> f32_;
  std::vector<int32_t> i32_;
  std::vector<int64_t> index_;
};

/* Run one invocation of `in` reading buffer `b` and writing to `out`.
 *
 * @return  A value derived from the output, so the work is not dead code
 */
inline float runOp(const OpInputs& in, int b, void* out) {
  const OpSpec& s = in.spec();
  const int64_t n = s.rows * s.cols;
  const float* x = in.f32(b);
  float* y = static_cast<float*>(out);
  switch (s.kind) {
    case OpKind::Copy:
      copyKernel(y, x, n);
      break;
    case OpKind::Fill:
      fillKernel(y, x[0], n);
      break;
    case OpKind::Add:
      addKernel(y, x, x + n, 0.5f, n);
      break;
    case OpKind::LayerNorm:
      layerNormKernel(y, x, x + n, x + n + s.cols, s.rows, s.cols, 1e-5f);
      break;
    case OpKind::SearchSorted:
      searchSortedKernel<float>(
          static_cast<int64_t*>(out), x + n, x, s.rows, s.aux, s.cols);
      return static_cast<float>(static_cast<int64_t*>(out)[n - 1]);
    case OpKind::ClampRangeLengths:
      clampRangeLengthsKernel(static_cast<int32_t*>(out), in.i32(b), n, 16);
      return static_cast<float>(static_cast<int32_t*>(out)[2 * n - 1]);
    case OpKind::CompleteCumsum:
      completeCumsumKernel(static_cast<int64_t*>(out), in.i32(b), n);
      return static_cast<float>(static_cast<int64_t*>(out)[n]);
    case OpKind::Diff:
      diffKernel(y, x, n);
      return y[n - 2];
    case OpKind::IndexSelect:
      indexSelectKernel(y, x, in.index(b), s.rows, s.cols);
      break;
    case OpKind::Cat:
      catKernel(y, x, s.rows, s.cols, s.aux);
      return y[n * s.aux - 1];
    case OpKind::ToHalf:
      toHalfKernel(static_cast<uint16_t*>(out), x, n);
      return static_cast<float>(static_cast<uint16_t*>(out)[n - 1]);
  }
  return y[n - 1];
}

} // namespace facebook::cea::chips::adsim::tensor_ops
