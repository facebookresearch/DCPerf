/*
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
*/

#if defined(__aarch64__)
#include <arm_neon.h>
#include <arm_sve.h>
#include <sys/auxv.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#include <xmmintrin.h>
#else
#error "Unsupported architecture"
#endif
#include <benchmark/benchmark.h>
#include <unistd.h>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstring>

class FPTypeConv : public benchmark::Fixture {
 public:
  static const size_t buf_size =
      18 * 1024; // 18KB to support unrolling of 2, 4, 6 and 8
  static_assert(buf_size % 3 == 0 && buf_size % 8 == 0);
  static const size_t alignment = 4096; // 4KB-aligned
  static const uint32_t crc32_fp32_to_fp16_validation_result = 3373450706;
  static const uint32_t crc32_fp16_to_fp32_validation_result = 2626649712;
  static const uint32_t crc32_fp32_to_bf16_validation_result = 2148563683;
  static const uint32_t crc32_bf16_to_fp32_validation_result = 4258514672;
  static const uint32_t crc32_fp32_to_uint8_validation_result = 3504027484;
  static const uint32_t crc32_uint8_to_fp32_validation_result = 736930253;

  float* fp32_buf = nullptr;
  uint16_t* half_buf = nullptr;
  uint8_t* uint8_buf = nullptr;

  void SetUp(const ::benchmark::State&) override {
    size_t n = buf_size / sizeof(float);

    fp32_buf = (float*)aligned_alloc(alignment, buf_size);
    half_buf = (uint16_t*)aligned_alloc(alignment, buf_size);
    memset(half_buf, 0, buf_size);
    uint8_buf = (uint8_t*)aligned_alloc(alignment, buf_size);
    memset(uint8_buf, 0, buf_size);
    // Touch memory to initialize and warm up cache
    for (size_t i = 0; i < n; i++) {
      fp32_buf[i] = (1.123456789f + (static_cast<float>(i) / 1000));
      half_buf[i] = static_cast<uint16_t>(0x3c00 + i);
      uint8_buf[i] = static_cast<uint8_t>(i);
    }

    fesetround(FE_TONEAREST);
  }

  void TearDown(const ::benchmark::State&) override {
    free(fp32_buf);
    free(half_buf);
    free(uint8_buf);
  }

  static uint32_t crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;

    if (!init) {
      for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
          c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
        table[i] = c;
      }
      init = true;
    }

    uint32_t crc = ~0u;
    while (len--)
      crc = (crc >> 1) ^ table[(crc ^ *data++) & 0xFF];
    return ~crc;
  }

  void check_result(
      benchmark::State& state,
      const uint8_t* buf,
      size_t size,
      uint32_t ref) {
    auto result = crc32(buf, size);
    if (result != ref) {
      std::string msg = "Skipping: result validation failed (expected: " +
          std::to_string(ref) + ", actual: " + std::to_string(result) + ")";
      state.SkipWithError(msg.c_str());
    }
  }
};

#if defined(__aarch64__)

// Query CPU feature support using getauxval on ARM64
struct CpuFeatures {
  bool fp16;
  bool bf16;
  bool sve;

  CpuFeatures() : fp16(false), bf16(false), sve(false) {
    // Query HWCAP (AT_HWCAP)
    unsigned long hwcap = getauxval(AT_HWCAP);

    // Check FP16 support (FEAT_FP16)
    // Both HWCAP_FPHP and HWCAP_ASIMDHP should be present for full FP16 support
#ifdef HWCAP_FPHP
    fp16 = (hwcap & HWCAP_FPHP) && (hwcap & HWCAP_ASIMDHP);
#endif

    // Check SVE support (FEAT_SVE)
#ifdef HWCAP_SVE
    sve = (hwcap & HWCAP_SVE);
#endif

    // Query HWCAP2 (AT_HWCAP2)
    unsigned long hwcap2 = getauxval(AT_HWCAP2);

    // Check BF16 support (FEAT_BF16)
#ifdef HWCAP2_BF16
    bf16 = (hwcap2 & HWCAP2_BF16);
#endif
  }
};

static CpuFeatures Cpu;

template <int unroll>
__attribute__((target("fp16"))) __attribute__((noinline)) void
fp32_to_fp16_neon(
    const float* __restrict__ fp32_buf,
    float16_t* __restrict__ half_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = 4;

#define PROCESS_VECTOR_PAIR(idx)                                           \
  float32x4_t f##idx##_0 = vld1q_f32(fp32_buf + i + (idx * 2 + 0) * vl);   \
  float32x4_t f##idx##_1 = vld1q_f32(fp32_buf + i + (idx * 2 + 1) * vl);   \
  float16x4_t h##idx##_low = vcvt_f16_f32(f##idx##_0);                     \
  float16x8_t h##idx##_full = vcvt_high_f16_f32(h##idx##_low, f##idx##_1); \
  vst1q_f16(half_buf + i + (idx * 2) * vl, h##idx##_full);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP32_TO_FP16_NEON(UC)                                              \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_fp16_##UC##_neon)(benchmark::State & state) {    \
    if (!Cpu.fp16) {                                                       \
      state.SkipWithError("Skipping: CPU does not support FEAT_FP16");     \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_fp16_neon<UC>(                                               \
          fp32_buf, reinterpret_cast<float16_t*>(half_buf), n_elem);       \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_fp16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_FP16_NEON(2)
FP32_TO_FP16_NEON(4)
FP32_TO_FP16_NEON(6)
FP32_TO_FP16_NEON(8)

template <int unroll>
__attribute__((target("fp16"))) __attribute__((noinline)) void
fp16_to_fp32_neon(
    const float16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vhl = 8;
  const size_t vwl = vhl / 2;

#define PROCESS_VECTOR_PAIR(idx)                                   \
  float16x8_t h##idx##_0 = vld1q_f16(half_buf + i + idx * vhl);    \
  float32x4_t f##idx##_0 = vcvt_f32_f16(vget_low_f16(h##idx##_0)); \
  float32x4_t f##idx##_1 = vcvt_high_f32_f16(h##idx##_0);          \
  vst1q_f32(fp32_buf + i + (idx * 2 + 0) * vwl, f##idx##_0);       \
  vst1q_f32(fp32_buf + i + (idx * 2 + 1) * vwl, f##idx##_1);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP16_TO_FP32_NEON(UC)                                              \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp16_to_fp32_##UC##_neon)(benchmark::State & state) {    \
    if (!Cpu.fp16) {                                                       \
      state.SkipWithError("Skipping: CPU does not support FEAT_FP16");     \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp16_to_fp32_neon<UC>(                                               \
          reinterpret_cast<float16_t*>(half_buf), fp32_buf, n_elem);       \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_fp16_to_fp32_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP16_TO_FP32_NEON(2)
FP16_TO_FP32_NEON(4)
FP16_TO_FP32_NEON(6)
FP16_TO_FP32_NEON(8)

template <int unroll, bool combined_write>
__attribute__((target("sve"))) __attribute__((noinline)) void fp32_to_fp16_sve(
    const float* __restrict__ fp32_buf,
    uint16_t* __restrict__ half_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = svcntw();
  svbool_t pg = svptrue_b16();

#define PROCESS_VECTOR_PAIR(idx)                                             \
  svfloat32_t f##idx##_0 = svld1_f32(pg, fp32_buf + i + (idx * 2 + 0) * vl); \
  svfloat32_t f##idx##_1 = svld1_f32(pg, fp32_buf + i + (idx * 2 + 1) * vl); \
  svfloat16_t h##idx##_0 = svcvt_f16_f32_x(pg, f##idx##_0);                  \
  svfloat16_t h##idx##_1 = svcvt_f16_f32_x(pg, f##idx##_1);                  \
  if constexpr (combined_write) {                                            \
    svfloat16_t h##idx##_packed = svuzp1_f16(h##idx##_0, h##idx##_1);        \
    svst1_f16(                                                               \
        pg,                                                                  \
        reinterpret_cast<float16_t*>(half_buf) + i + (idx * 2) * vl,         \
        h##idx##_packed);                                                    \
  } else {                                                                   \
    svst1h_u32(                                                              \
        pg,                                                                  \
        half_buf + i + (idx * 2 + 0) * vl,                                   \
        svreinterpret_u32_f16(h##idx##_0));                                  \
    svst1h_u32(                                                              \
        pg,                                                                  \
        half_buf + i + (idx * 2 + 1) * vl,                                   \
        svreinterpret_u32_f16(h##idx##_1));                                  \
  }

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP32_TO_FP16_CW_SVE(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_fp16_cw_##UC##_sve)(benchmark::State & state) {  \
    if (!Cpu.sve) {                                                        \
      state.SkipWithError("Skipping: CPU does not support SVE");           \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_fp16_sve<UC, true>(fp32_buf, half_buf, n_elem);              \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_fp16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_FP16_CW_SVE(2)
FP32_TO_FP16_CW_SVE(4)
FP32_TO_FP16_CW_SVE(6)
FP32_TO_FP16_CW_SVE(8)

#define FP32_TO_FP16_SVE(UC)                                                   \
  BENCHMARK_F(FPTypeConv, fp32_to_fp16_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve) {                                                            \
      state.SkipWithError("Skipping: CPU does not support SVE");               \
      return;                                                                  \
    }                                                                          \
    const size_t n_elem = buf_size / sizeof(float);                            \
    for (auto _ : state) {                                                     \
      fp32_to_fp16_sve<UC, false>(fp32_buf, half_buf, n_elem);                 \
    }                                                                          \
    check_result(                                                              \
        state,                                                                 \
        reinterpret_cast<uint8_t*>(half_buf),                                  \
        buf_size,                                                              \
        crc32_fp32_to_fp16_validation_result);                                 \
    state.counters["elem/s"] = benchmark::Counter(                             \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);     \
  }

FP32_TO_FP16_SVE(2)
FP32_TO_FP16_SVE(4)
FP32_TO_FP16_SVE(6)
FP32_TO_FP16_SVE(8)

template <int unroll>
__attribute__((target("sve"))) __attribute__((noinline)) void fp16_to_fp32_sve(
    const float16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vhl = svcnth();
  const size_t vwl = vhl / 2;
  svbool_t pg = svptrue_b32();

#define PROCESS_VECTOR_PAIR(idx)                                      \
  svfloat16_t h##idx##_0 = svreinterpret_f16_s32(                     \
      svld1sh_s32(pg, (int16_t*)half_buf + i + (idx * 2 + 0) * vwl)); \
  svfloat16_t h##idx##_1 = svreinterpret_f16_s32(                     \
      svld1sh_s32(pg, (int16_t*)half_buf + i + (idx * 2 + 1) * vwl)); \
  svfloat32_t f##idx##_0 = svcvt_f32_f16_x(pg, h##idx##_0);           \
  svfloat32_t f##idx##_1 = svcvt_f32_f16_x(pg, h##idx##_1);           \
  svst1_f32(pg, fp32_buf + i + (idx * 2 + 0) * vwl, f##idx##_0);      \
  svst1_f32(pg, fp32_buf + i + (idx * 2 + 1) * vwl, f##idx##_1);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP16_TO_FP32_SVE(UC)                                                   \
  BENCHMARK_F(FPTypeConv, fp16_to_fp32_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve) {                                                            \
      state.SkipWithError("Skipping: CPU does not support SVE");               \
      return;                                                                  \
    }                                                                          \
    const size_t n_elem = buf_size / sizeof(float);                            \
    for (auto _ : state) {                                                     \
      fp16_to_fp32_sve<UC>(                                                    \
          reinterpret_cast<float16_t*>(half_buf), fp32_buf, n_elem);           \
    }                                                                          \
    check_result(                                                              \
        state,                                                                 \
        reinterpret_cast<uint8_t*>(fp32_buf),                                  \
        buf_size,                                                              \
        crc32_fp16_to_fp32_validation_result);                                 \
    state.counters["elem/s"] = benchmark::Counter(                             \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);     \
  }

FP16_TO_FP32_SVE(2)
FP16_TO_FP32_SVE(4)
FP16_TO_FP32_SVE(6)
FP16_TO_FP32_SVE(8)

template <int unroll>
__attribute__((target("bf16"))) __attribute__((noinline)) void
fp32_to_bf16_neon(
    const float* __restrict__ fp32_buf,
    bfloat16_t* __restrict__ half_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = 4;

#define PROCESS_VECTOR_PAIR(idx)                                         \
  float32x4_t f##idx##_0 = vld1q_f32(fp32_buf + i + (idx * 2 + 0) * vl); \
  float32x4_t f##idx##_1 = vld1q_f32(fp32_buf + i + (idx * 2 + 1) * vl); \
  bfloat16x8_t h##idx##_full = vcvtq_low_bf16_f32(f##idx##_0);           \
  h##idx##_full = vcvtq_high_bf16_f32(h##idx##_full, f##idx##_1);        \
  vst1q_bf16(half_buf + i + (idx * 2) * vl, h##idx##_full);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP32_TO_BF16_NEON(UC)                                              \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_bf16_##UC##_neon)(benchmark::State & state) {    \
    if (!Cpu.bf16) {                                                       \
      state.SkipWithError("Skipping: CPU does not support FEAT_BF16");     \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_bf16_neon<UC>(                                               \
          fp32_buf, reinterpret_cast<bfloat16_t*>(half_buf), n_elem);      \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_bf16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_BF16_NEON(2)
FP32_TO_BF16_NEON(4)
FP32_TO_BF16_NEON(6)
FP32_TO_BF16_NEON(8)

template <int unroll, bool packing>
__attribute__((target("bf16"))) __attribute__((noinline)) void
bf16_to_fp32_neon(
    const bfloat16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vhl = 8;
  const size_t vwl = vhl / 2;
  [[maybe_unused]] uint16x8_t hz = vdupq_n_u16(0);

#define PROCESS_VECTOR_PAIR(idx)                                  \
  bfloat16x8_t h##idx##_0 = vld1q_bf16(half_buf + i + idx * vhl); \
  float32x4_t f##idx##_0, f##idx##_1;                             \
  if constexpr (packing) {                                        \
    f##idx##_0 = vreinterpretq_f32_u16(                           \
        vzip1q_u16(hz, vreinterpretq_u16_bf16(h##idx##_0)));      \
    f##idx##_1 = vreinterpretq_f32_u16(                           \
        vzip2q_u16(hz, vreinterpretq_u16_bf16(h##idx##_0)));      \
  } else {                                                        \
    f##idx##_0 = vcvtq_low_f32_bf16(h##idx##_0);                  \
    f##idx##_1 = vcvtq_high_f32_bf16(h##idx##_0);                 \
  }                                                               \
  vst1q_f32(fp32_buf + i + (idx * 2 + 0) * vwl, f##idx##_0);      \
  vst1q_f32(fp32_buf + i + (idx * 2 + 1) * vwl, f##idx##_1);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define BF16_TO_FP32_PK_NEON(UC)                                           \
  BENCHMARK_F(                                                             \
      FPTypeConv, bf16_to_fp32_##UC##_pk_neon)(benchmark::State & state) { \
    if (!Cpu.bf16) {                                                       \
      state.SkipWithError("Skipping: CPU does not support FEAT_BF16");     \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      bf16_to_fp32_neon<UC, true>(                                         \
          reinterpret_cast<bfloat16_t*>(half_buf), fp32_buf, n_elem);      \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_bf16_to_fp32_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

BF16_TO_FP32_PK_NEON(2)
BF16_TO_FP32_PK_NEON(4)
BF16_TO_FP32_PK_NEON(6)
BF16_TO_FP32_PK_NEON(8)

#define BF16_TO_FP32_NEON(UC)                                              \
  BENCHMARK_F(                                                             \
      FPTypeConv, bf16_to_fp32_##UC##_neon)(benchmark::State & state) {    \
    if (!Cpu.bf16) {                                                       \
      state.SkipWithError("Skipping: CPU does not support FEAT_BF16");     \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      bf16_to_fp32_neon<UC, false>(                                        \
          reinterpret_cast<bfloat16_t*>(half_buf), fp32_buf, n_elem);      \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_bf16_to_fp32_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

BF16_TO_FP32_NEON(2)
BF16_TO_FP32_NEON(4)
BF16_TO_FP32_NEON(6)
BF16_TO_FP32_NEON(8)

template <int unroll, bool combined_write>
__attribute__((target("sve, bf16"))) __attribute__((noinline)) void
fp32_to_bf16_sve(
    const float* __restrict__ fp32_buf,
    uint16_t* __restrict__ half_buf,
    size_t n_elem) {
  const size_t vl = svcntw();
  size_t i;
  svbool_t pg = svptrue_b16();

#define PROCESS_VECTOR_PAIR(idx)                                             \
  svfloat32_t f##idx##_0 = svld1_f32(pg, fp32_buf + i + (idx * 2 + 0) * vl); \
  svfloat32_t f##idx##_1 = svld1_f32(pg, fp32_buf + i + (idx * 2 + 1) * vl); \
  svbfloat16_t h##idx##_0 = svcvt_bf16_f32_x(pg, f##idx##_0);                \
  svbfloat16_t h##idx##_1 = svcvt_bf16_f32_x(pg, f##idx##_1);                \
  if constexpr (combined_write) {                                            \
    svbfloat16_t h##idx##_packed = svuzp1_bf16(h##idx##_0, h##idx##_1);      \
    svst1_bf16(                                                              \
        pg,                                                                  \
        reinterpret_cast<bfloat16_t*>(half_buf) + i + (idx * 2) * vl,        \
        h##idx##_packed);                                                    \
  } else {                                                                   \
    svst1h_u32(                                                              \
        pg,                                                                  \
        half_buf + i + (idx * 2 + 0) * vl,                                   \
        svreinterpret_u32_bf16(h##idx##_0));                                 \
    svst1h_u32(                                                              \
        pg,                                                                  \
        half_buf + i + (idx * 2 + 1) * vl,                                   \
        svreinterpret_u32_bf16(h##idx##_1));                                 \
  }

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP32_TO_BF16_CW_SVE(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_bf16_cw_##UC##_sve)(benchmark::State & state) {  \
    if (!Cpu.sve || !Cpu.bf16) {                                           \
      state.SkipWithError("Skipping: CPU does not support SVE+FEAT_BF16"); \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_bf16_sve<UC, true>(fp32_buf, half_buf, n_elem);              \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_bf16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_BF16_CW_SVE(2)
FP32_TO_BF16_CW_SVE(4)
FP32_TO_BF16_CW_SVE(6)
FP32_TO_BF16_CW_SVE(8)

#define FP32_TO_BF16_SVE(UC)                                                   \
  BENCHMARK_F(FPTypeConv, fp32_to_bf16_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve || !Cpu.bf16) {                                               \
      state.SkipWithError("Skipping: CPU does not support SVE+FEAT_BF16");     \
      return;                                                                  \
    }                                                                          \
    const size_t n_elem = buf_size / sizeof(float);                            \
    for (auto _ : state) {                                                     \
      fp32_to_bf16_sve<UC, false>(fp32_buf, half_buf, n_elem);                 \
    }                                                                          \
    check_result(                                                              \
        state,                                                                 \
        reinterpret_cast<uint8_t*>(half_buf),                                  \
        buf_size,                                                              \
        crc32_fp32_to_bf16_validation_result);                                 \
    state.counters["elem/s"] = benchmark::Counter(                             \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);     \
  }

FP32_TO_BF16_SVE(2)
FP32_TO_BF16_SVE(4)
FP32_TO_BF16_SVE(6)
FP32_TO_BF16_SVE(8)

template <int unroll>
__attribute__((target("sve2, bf16"))) __attribute__((noinline)) void
bf16_to_fp32_sve(
    const bfloat16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vhl = svcnth();
  const size_t vwl = vhl / 2;
  svbool_t pg = svptrue_b16();
  [[maybe_unused]] svbfloat16_t hz = svreinterpret_bf16_u16(svdup_u16(0));

#define PROCESS_VECTOR_PAIR(idx)                                      \
  svbfloat16_t h##idx##_0 = svld1_bf16(pg, half_buf + i + idx * vhl); \
  svfloat32_t f##idx##_0, f##idx##_1;                                 \
  f##idx##_0 = svreinterpret_f32_bf16(svzip1_bf16(hz, h##idx##_0));   \
  f##idx##_1 = svreinterpret_f32_bf16(svzip2_bf16(hz, h##idx##_0));   \
  svst1_f32(pg, fp32_buf + i + (idx * 2 + 0) * vwl, f##idx##_0);      \
  svst1_f32(pg, fp32_buf + i + (idx * 2 + 1) * vwl, f##idx##_1);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
      PROCESS_VECTOR_PAIR(3)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
      PROCESS_VECTOR_PAIR(2)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
      PROCESS_VECTOR_PAIR(1)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vwl) {
      PROCESS_VECTOR_PAIR(0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define BF16_TO_FP32_SVE(UC)                                                   \
  BENCHMARK_F(FPTypeConv, bf16_to_fp32_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve || !Cpu.bf16) {                                               \
      state.SkipWithError("Skipping: CPU does not support SVE+FEAT_BF16");     \
      return;                                                                  \
    }                                                                          \
    const size_t n_elem = buf_size / sizeof(float);                            \
    for (auto _ : state) {                                                     \
      bf16_to_fp32_sve<UC>(                                                    \
          reinterpret_cast<bfloat16_t*>(half_buf), fp32_buf, n_elem);          \
    }                                                                          \
    check_result(                                                              \
        state,                                                                 \
        reinterpret_cast<uint8_t*>(fp32_buf),                                  \
        buf_size,                                                              \
        crc32_bf16_to_fp32_validation_result);                                 \
    state.counters["elem/s"] = benchmark::Counter(                             \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);     \
  }

BF16_TO_FP32_SVE(2)
BF16_TO_FP32_SVE(4)
BF16_TO_FP32_SVE(6)
BF16_TO_FP32_SVE(8)

template <int unroll, bool saturate>
__attribute__((noinline)) void fp32_rz_to_u8_neon(
    const float* __restrict__ fp32_buf,
    uint8_t* __restrict__ uint8_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = 4;

#define PROCESS_VECTOR_PAIR(idx, f0, f1, i0, i1, i01, i01_half)  \
  float32x4_t f0 = vld1q_f32(fp32_buf + i + (idx * 2 * vl));     \
  float32x4_t f1 = vld1q_f32(fp32_buf + i + (idx * 2 + 1) * vl); \
  int32x4_t i0 = vcvtq_s32_f32(f0);                              \
  int32x4_t i1 = vcvtq_s32_f32(f1);                              \
  uint16x8_t i01;                                                \
  uint8x8_t i01_half;                                            \
  if constexpr (saturate) {                                      \
    i01 = vcombine_u16(vqmovun_s32(i0), vqmovun_s32(i1));        \
    i01_half = vqmovn_u16(i01);                                  \
  } else {                                                       \
    i01 = vcombine_u16(vmovn_u32(i0), vmovn_u32(i1));            \
    i01_half = vmovn_u16(i01);                                   \
  }                                                              \
  vst1_u8(uint8_buf + i + idx * 8, i01_half);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0, f0, f1, i0, i1, i01, i01_half)
      PROCESS_VECTOR_PAIR(1, f2, f3, i2, i3, i23, i23_half)
      PROCESS_VECTOR_PAIR(2, f4, f5, i4, i5, i45, i45_half)
      PROCESS_VECTOR_PAIR(3, f6, f7, i6, i7, i67, i67_half)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0, f0, f1, i0, i1, i01, i01_half)
      PROCESS_VECTOR_PAIR(1, f2, f3, i2, i3, i23, i23_half)
      PROCESS_VECTOR_PAIR(2, f4, f5, i4, i5, i45, i45_half)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0, f0, f1, i0, i1, i01, i01_half)
      PROCESS_VECTOR_PAIR(1, f2, f3, i2, i3, i23, i23_half)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR_PAIR(0, f0, f1, i0, i1, i01, i01_half)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define FP32_RZ_TO_U8_SATURATE_NEON(UC)                                     \
  BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_saturate_##UC##_neon)(              \
      benchmark::State & state) {                                           \
    const size_t n_elem = buf_size / sizeof(float);                         \
    for (auto _ : state) {                                                  \
      fp32_rz_to_u8_neon<UC, true>(fp32_buf, uint8_buf, n_elem);            \
    }                                                                       \
    check_result(                                                           \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result); \
    state.counters["elem/s"] = benchmark::Counter(                          \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);  \
  }

FP32_RZ_TO_U8_SATURATE_NEON(2)
FP32_RZ_TO_U8_SATURATE_NEON(4)
FP32_RZ_TO_U8_SATURATE_NEON(6)
FP32_RZ_TO_U8_SATURATE_NEON(8)

#define FP32_RZ_TO_U8_NARROW_NEON(UC)                                       \
  BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_narrow_##UC##_neon)(                \
      benchmark::State & state) {                                           \
    const size_t n_elem = buf_size / sizeof(float);                         \
    for (auto _ : state) {                                                  \
      fp32_rz_to_u8_neon<UC, false>(fp32_buf, uint8_buf, n_elem);           \
    }                                                                       \
    check_result(                                                           \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result); \
    state.counters["elem/s"] = benchmark::Counter(                          \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);  \
  }

FP32_RZ_TO_U8_NARROW_NEON(2)
FP32_RZ_TO_U8_NARROW_NEON(4)
FP32_RZ_TO_U8_NARROW_NEON(6)
FP32_RZ_TO_U8_NARROW_NEON(8)

template <int unroll>
__attribute__((noinline)) void u8_to_fp32_neon(
    const uint8_t* __restrict__ uint8_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = 16;

  const uint8x16_t idx_0 = {
      0, 255, 255, 255, 1, 255, 255, 255, 2, 255, 255, 255, 3, 255, 255, 255};
  const uint8x16_t idx_1 = {
      4, 255, 255, 255, 5, 255, 255, 255, 6, 255, 255, 255, 7, 255, 255, 255};
  const uint8x16_t idx_2 = {
      8, 255, 255, 255, 9, 255, 255, 255, 10, 255, 255, 255, 11, 255, 255, 255};
  const uint8x16_t idx_3 = {
      12,
      255,
      255,
      255,
      13,
      255,
      255,
      255,
      14,
      255,
      255,
      255,
      15,
      255,
      255,
      255};

#define PROCESS_VECTOR_PAIR(i0, idx0, idx1, offset)               \
  {                                                               \
    uint32x4_t i0_0 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx0)); \
    uint32x4_t i0_1 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx1)); \
    float32x4_t f0 = vcvtq_f32_u32(i0_0);                         \
    float32x4_t f1 = vcvtq_f32_u32(i0_1);                         \
    vst1q_f32(fp32_buf + i + offset, f0);                         \
    vst1q_f32(fp32_buf + i + offset + 4, f1);                     \
  }

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += vl * 2) {
      uint8x16_t i0 = vld1q_u8(uint8_buf + i);
      uint8x16_t i1 = vld1q_u8(uint8_buf + i + 16);
      PROCESS_VECTOR_PAIR(i0, idx_0, idx_1, 0)
      PROCESS_VECTOR_PAIR(i0, idx_2, idx_3, 8)
      PROCESS_VECTOR_PAIR(i1, idx_0, idx_1, 16)
      PROCESS_VECTOR_PAIR(i1, idx_2, idx_3, 24)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += vl + vl / 2) {
      // Make sure the three reads are all aligned.
      uint8x16_t i0 = vcombine_u8(vld1_u8(uint8_buf + i), vdup_n_u8(0));
      uint8x16_t i1 = vcombine_u8(vld1_u8(uint8_buf + i + 8), vdup_n_u8(0));
      uint8x16_t i2 = vcombine_u8(vld1_u8(uint8_buf + i + 16), vdup_n_u8(0));

      PROCESS_VECTOR_PAIR(i0, idx_0, idx_1, 0)
      PROCESS_VECTOR_PAIR(i1, idx_0, idx_1, 8)
      PROCESS_VECTOR_PAIR(i2, idx_0, idx_1, 16)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += vl) {
      uint8x16_t i0 = vld1q_u8(uint8_buf + i);

      PROCESS_VECTOR_PAIR(i0, idx_0, idx_1, 0)
      PROCESS_VECTOR_PAIR(i0, idx_2, idx_3, 8)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += vl / 2) {
      uint8x16_t i0 = vcombine_u8(vld1_u8(uint8_buf + i), vdup_n_u8(0));

      PROCESS_VECTOR_PAIR(i0, idx_0, idx_1, 0)
    }
  }

#undef PROCESS_VECTOR_PAIR
}

#define U8_TO_FP32_NEON(UC)                                                   \
  BENCHMARK_F(FPTypeConv, u8_to_fp32_##UC##_neon)(benchmark::State & state) { \
    const size_t n_elem = buf_size / sizeof(float);                           \
    for (auto _ : state) {                                                    \
      u8_to_fp32_neon<UC>(uint8_buf, fp32_buf, n_elem);                       \
    }                                                                         \
    check_result(                                                             \
        state,                                                                \
        reinterpret_cast<uint8_t*>(fp32_buf),                                 \
        buf_size,                                                             \
        crc32_uint8_to_fp32_validation_result);                               \
    state.counters["elem/s"] = benchmark::Counter(                            \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);    \
  }

U8_TO_FP32_NEON(2)
U8_TO_FP32_NEON(4)
U8_TO_FP32_NEON(6)
U8_TO_FP32_NEON(8)

template <int unroll, bool saturate>
__attribute__((target("sve2"))) __attribute__((noinline)) void
fp32_rz_to_u8_sve(
    const float* __restrict__ fp32_buf,
    uint8_t* __restrict__ uint8_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl = svcntw();
  svbool_t pg = svptrue_b32();
  [[maybe_unused]] svint32_t zero = svdup_s32(0);
  [[maybe_unused]] svint32_t two_fifty_five = svdup_s32(255);
#define PROCESS_VECTOR(idx)                                    \
  svfloat32_t f##idx = svld1_f32(pg, fp32_buf + i + idx * vl); \
  svint32_t i##idx = svcvt_s32_f32_x(pg, f##idx);              \
  if constexpr (saturate) {                                    \
    i##idx = svmin_s32_x(pg, i##idx, two_fifty_five);          \
    i##idx = svmax_s32_x(pg, i##idx, zero);                    \
  }                                                            \
  svst1b_s32(pg, reinterpret_cast<int8_t*>(uint8_buf) + i + idx * vl, i##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define FP32_RZ_TO_U8_SATURATE_SVE(UC)                                      \
  BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_saturate_##UC##_sve)(               \
      benchmark::State & state) {                                           \
    if (!Cpu.sve) {                                                         \
      state.SkipWithError("Skipping: CPU does not support SVE");            \
      return;                                                               \
    }                                                                       \
    const size_t n_elem = buf_size / sizeof(float);                         \
    for (auto _ : state) {                                                  \
      fp32_rz_to_u8_sve<UC, true>(fp32_buf, uint8_buf, n_elem);             \
    }                                                                       \
    check_result(                                                           \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result); \
    state.counters["elem/s"] = benchmark::Counter(                          \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);  \
  }

FP32_RZ_TO_U8_SATURATE_SVE(2)
FP32_RZ_TO_U8_SATURATE_SVE(4)
FP32_RZ_TO_U8_SATURATE_SVE(6)
FP32_RZ_TO_U8_SATURATE_SVE(8)

#define FP32_RZ_TO_U8_NARROW_SVE(UC)                                           \
  BENCHMARK_F(                                                                 \
      FPTypeConv, fp32_rz_to_u8_narrow_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve) {                                                            \
      state.SkipWithError("Skipping: CPU does not support SVE");               \
      return;                                                                  \
    }                                                                          \
    const size_t n_elem = buf_size / sizeof(float);                            \
    for (auto _ : state) {                                                     \
      fp32_rz_to_u8_sve<UC, false>(fp32_buf, uint8_buf, n_elem);               \
    }                                                                          \
    check_result(                                                              \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);    \
    state.counters["elem/s"] = benchmark::Counter(                             \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);     \
  }

FP32_RZ_TO_U8_NARROW_SVE(2)
FP32_RZ_TO_U8_NARROW_SVE(4)
FP32_RZ_TO_U8_NARROW_SVE(6)
FP32_RZ_TO_U8_NARROW_SVE(8)

template <int unroll>
__attribute__((target("sve"))) __attribute__((noinline)) void u8_to_fp32_sve(
    const uint8_t* __restrict__ uint8_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  size_t i;
  const size_t vl_s32 = svcntw();
  svbool_t pg = svptrue_b32();

#define PROCESS_VECTOR(idx)                                          \
  svuint32_t i##idx = svld1ub_u32(pg, uint8_buf + i + idx * vl_s32); \
  svfloat32_t f##idx = svcvt_f32_u32_x(pg, i##idx);                  \
  svst1_f32(pg, fp32_buf + i + idx * vl_s32, f##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl_s32) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl_s32) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl_s32) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl_s32) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define U8_TO_FP32_SVE(UC)                                                   \
  BENCHMARK_F(FPTypeConv, u8_to_fp32_##UC##_sve)(benchmark::State & state) { \
    if (!Cpu.sve) {                                                          \
      state.SkipWithError("Skipping: CPU does not support SVE");             \
      return;                                                                \
    }                                                                        \
    const size_t n_elem = buf_size / sizeof(float);                          \
    for (auto _ : state) {                                                   \
      u8_to_fp32_sve<UC>(uint8_buf, fp32_buf, n_elem);                       \
    }                                                                        \
    check_result(                                                            \
        state,                                                               \
        reinterpret_cast<uint8_t*>(fp32_buf),                                \
        buf_size,                                                            \
        crc32_uint8_to_fp32_validation_result);                              \
    state.counters["elem/s"] = benchmark::Counter(                           \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);   \
  }

U8_TO_FP32_SVE(2)
U8_TO_FP32_SVE(4)
U8_TO_FP32_SVE(6)
U8_TO_FP32_SVE(8)

#else

// Query CPU feature support using getauxval on ARM64
struct CpuFeatures {
  bool avx512f;
  bool avx512bf16;

  CpuFeatures() : avx512f(false), avx512bf16(false) {
    avx512f = __builtin_cpu_supports("avx512f");
    avx512bf16 = __builtin_cpu_supports("avx512bf16");
  }
};

static CpuFeatures Cpu;

template <int unroll>
__attribute__((target("avx512f"))) __attribute__((noinline)) void
fp32_to_fp16_avx512(
    const float* __restrict__ fp32_buf,
    uint16_t* __restrict__ half_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR(idx)                                 \
  __m512 f##idx = _mm512_loadu_ps(fp32_buf + i + idx * vl); \
  __m256i h##idx = _mm512_cvtps_ph(f##idx, 0);              \
  _mm256_storeu_si256(                                      \
      reinterpret_cast<__m256i*>(&half_buf[i + idx * vl]), h##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define FP32_TO_FP16_AVX512(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_fp16_##UC##_avx512)(benchmark::State & state) {  \
    if (!Cpu.avx512f) {                                                    \
      state.SkipWithError("Skipping: CPU does not support AVX512F");       \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_fp16_avx512<UC>(fp32_buf, half_buf, n_elem);                 \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_fp16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_FP16_AVX512(2)
FP32_TO_FP16_AVX512(4)
FP32_TO_FP16_AVX512(6)
FP32_TO_FP16_AVX512(8)

template <int unroll>
__attribute__((target("avx512f"))) __attribute__((noinline)) void
fp16_to_fp32_avx512(
    const uint16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR(idx)                                       \
  __m256i h##idx = _mm256_loadu_si256(                            \
      reinterpret_cast<const __m256i*>(&half_buf[i + idx * vl])); \
  __m512 f##idx = _mm512_cvtph_ps(h##idx);                        \
  _mm512_storeu_ps(fp32_buf + i + idx * vl, f##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define FP16_TO_FP32_AVX512(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp16_to_fp32_##UC##_avx512)(benchmark::State & state) {  \
    if (!Cpu.avx512f) {                                                    \
      state.SkipWithError("Skipping: CPU does not support AVX512F");       \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp16_to_fp32_avx512<UC>(half_buf, fp32_buf, n_elem);                 \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_fp16_to_fp32_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP16_TO_FP32_AVX512(2)
FP16_TO_FP32_AVX512(4)
FP16_TO_FP32_AVX512(6)
FP16_TO_FP32_AVX512(8)

template <int unroll>
__attribute__((target("avx512f, avx512bf16"))) __attribute__((noinline)) void
fp32_to_bf16_avx512(
    const float* __restrict__ fp32_buf,
    uint16_t* __restrict__ half_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR(idx)                                 \
  __m512 f##idx = _mm512_loadu_ps(fp32_buf + i + idx * vl); \
  __m256i h##idx = _mm512_cvtneps_pbh(f##idx);              \
  _mm256_storeu_si256(                                      \
      reinterpret_cast<__m256i*>(&half_buf[i + idx * vl]), h##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define FP32_TO_BF16_AVX512(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, fp32_to_bf16_##UC##_avx512)(benchmark::State & state) {  \
    if (!Cpu.avx512bf16) {                                                 \
      state.SkipWithError(                                                 \
          "Skipping: CPU does not support AVX512F+AVX512BF16");            \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      fp32_to_bf16_avx512<UC>(fp32_buf, half_buf, n_elem);                 \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(half_buf),                              \
        buf_size,                                                          \
        crc32_fp32_to_bf16_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

FP32_TO_BF16_AVX512(2)
FP32_TO_BF16_AVX512(4)
FP32_TO_BF16_AVX512(6)
FP32_TO_BF16_AVX512(8)

template <int unroll>
__attribute__((target("avx512f"))) __attribute__((noinline)) void
bf16_to_fp32_avx512(
    const uint16_t* __restrict__ half_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR(idx)                                       \
  __m256i h##idx = _mm256_loadu_si256(                            \
      reinterpret_cast<const __m256i*>(&half_buf[i + idx * vl])); \
  __m512i f##idx##_i = _mm512_cvtepu16_epi32(h##idx);             \
  f##idx##_i = _mm512_slli_epi32(f##idx##_i, 16);                 \
  _mm512_storeu_ps(fp32_buf + i + idx * vl, _mm512_castsi512_ps(f##idx##_i));

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define BF16_TO_FP32_AVX512(UC)                                            \
  BENCHMARK_F(                                                             \
      FPTypeConv, bf16_to_fp32_##UC##_avx512)(benchmark::State & state) {  \
    if (!Cpu.avx512f) {                                                    \
      state.SkipWithError("Skipping: CPU does not support AVX512F");       \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      bf16_to_fp32_avx512<UC>(half_buf, fp32_buf, n_elem);                 \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_bf16_to_fp32_validation_result);                             \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

BF16_TO_FP32_AVX512(2)
BF16_TO_FP32_AVX512(4)
BF16_TO_FP32_AVX512(6)
BF16_TO_FP32_AVX512(8)

template <int unroll, bool saturate>
__attribute__((target("avx512f, avx512bw"))) __attribute__((noinline)) void
fp32_rz_to_u8_avx512(
    const float* __restrict__ fp32_buf,
    uint8_t* __restrict__ uint8_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR_SATURATE(idx)                        \
  __m512 f##idx = _mm512_loadu_ps(fp32_buf + i + idx * vl); \
  __m512i i##idx = _mm512_cvt_roundps_epi32(                \
      f##idx, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);

#define PROCESS_VECTOR_NARROW(idx)                          \
  __m512 f##idx = _mm512_loadu_ps(fp32_buf + i + idx * vl); \
  __m512i i##idx = _mm512_cvt_roundps_epi32(                \
      f##idx, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);      \
  _mm512_mask_cvtusepi32_storeu_epi8(                       \
      reinterpret_cast<__m512i*>(uint8_buf + i + idx * vl), 0xFFFF, i##idx);

  if constexpr (saturate) {
    if constexpr (unroll == 8) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_SATURATE(0)
        PROCESS_VECTOR_SATURATE(1)
        PROCESS_VECTOR_SATURATE(2)
        PROCESS_VECTOR_SATURATE(3)
        PROCESS_VECTOR_SATURATE(4)
        PROCESS_VECTOR_SATURATE(5)
        PROCESS_VECTOR_SATURATE(6)
        PROCESS_VECTOR_SATURATE(7)

        __m512i i01_16 = _mm512_packus_epi32(i0, i1);
        __m512i i23_16 = _mm512_packus_epi32(i2, i3);
        __m512i i45_16 = _mm512_packus_epi32(i4, i5);
        __m512i i67_16 = _mm512_packus_epi32(i6, i7);

        __m512i i0123_8 = _mm512_packus_epi16(i01_16, i23_16);
        __m512i i4567_8 = _mm512_packus_epi16(i45_16, i67_16);

        const __m512i perm_idx = _mm512_setr_epi32(
            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        i0123_8 = _mm512_permutexvar_epi32(perm_idx, i0123_8);
        i4567_8 = _mm512_permutexvar_epi32(perm_idx, i4567_8);

        _mm512_storeu_si512(reinterpret_cast<__m512i*>(uint8_buf + i), i0123_8);
        _mm512_storeu_si512(
            reinterpret_cast<__m512i*>(uint8_buf + i + 4 * vl), i4567_8);
      }
    } else if constexpr (unroll == 6) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_SATURATE(0)
        PROCESS_VECTOR_SATURATE(1)
        PROCESS_VECTOR_SATURATE(2)
        PROCESS_VECTOR_SATURATE(3)
        PROCESS_VECTOR_SATURATE(4)
        PROCESS_VECTOR_SATURATE(5)

        __m512i i01_16 = _mm512_packus_epi32(i0, i1);
        __m512i i23_16 = _mm512_packus_epi32(i2, i3);
        __m512i i45_16 = _mm512_packus_epi32(i4, i5);

        __m512i i0123_8 = _mm512_packus_epi16(i01_16, i23_16);
        __m512i i45xx_8 = _mm512_packus_epi16(i45_16, i45_16);

        const __m512i perm_idx = _mm512_setr_epi32(
            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        i0123_8 = _mm512_permutexvar_epi32(perm_idx, i0123_8);
        i45xx_8 = _mm512_permutexvar_epi32(perm_idx, i45xx_8);

        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(uint8_buf + i),
            _mm512_castsi512_si256(i0123_8));
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(uint8_buf + i + 2 * vl),
            _mm512_extracti32x8_epi32(i0123_8, 1));
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(uint8_buf + i + 4 * vl),
            _mm512_castsi512_si256(i45xx_8));
      }
    } else if constexpr (unroll == 4) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_SATURATE(0)
        PROCESS_VECTOR_SATURATE(1)
        PROCESS_VECTOR_SATURATE(2)
        PROCESS_VECTOR_SATURATE(3)

        __m512i i01_16 = _mm512_packus_epi32(i0, i1);
        __m512i i23_16 = _mm512_packus_epi32(i2, i3);

        __m512i i0123_8 = _mm512_packus_epi16(i01_16, i23_16);

        const __m512i perm_idx = _mm512_setr_epi32(
            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        i0123_8 = _mm512_permutexvar_epi32(perm_idx, i0123_8);

        _mm512_storeu_si512(reinterpret_cast<__m512i*>(uint8_buf + i), i0123_8);
      }
    } else if constexpr (unroll == 2) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_SATURATE(0)
        PROCESS_VECTOR_SATURATE(1)

        __m512i i01_16 = _mm512_packus_epi32(i0, i1);
        __m512i i01_8 = _mm512_packus_epi16(i01_16, i01_16);

        const __m512i perm_idx = _mm512_setr_epi32(
            0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
        i01_8 = _mm512_permutexvar_epi32(perm_idx, i01_8);

        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(uint8_buf + i),
            _mm512_castsi512_si256(i01_8));
      }
    }
  } else {
    if constexpr (unroll == 8) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_NARROW(0)
        PROCESS_VECTOR_NARROW(1)
        PROCESS_VECTOR_NARROW(2)
        PROCESS_VECTOR_NARROW(3)
        PROCESS_VECTOR_NARROW(4)
        PROCESS_VECTOR_NARROW(5)
        PROCESS_VECTOR_NARROW(6)
        PROCESS_VECTOR_NARROW(7)
      }
    } else if constexpr (unroll == 6) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_NARROW(0)
        PROCESS_VECTOR_NARROW(1)
        PROCESS_VECTOR_NARROW(2)
        PROCESS_VECTOR_NARROW(3)
        PROCESS_VECTOR_NARROW(4)
        PROCESS_VECTOR_NARROW(5)
      }
    } else if constexpr (unroll == 4) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_NARROW(0)
        PROCESS_VECTOR_NARROW(1)
        PROCESS_VECTOR_NARROW(2)
        PROCESS_VECTOR_NARROW(3)
      }
    } else if constexpr (unroll == 2) {
      for (i = 0; i < n_elem; i += unroll * vl) {
        PROCESS_VECTOR_NARROW(0)
        PROCESS_VECTOR_NARROW(1)
      }
    }
  }

#undef PROCESS_VECTOR_SATURATE
#undef PROCESS_VECTOR_NARROW
}

#define FP32_RZ_TO_U8_SATURATE_AVX512(UC)                                   \
  BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_saturate_##UC##_avx512)(            \
      benchmark::State & state) {                                           \
    if (!Cpu.avx512f) {                                                     \
      state.SkipWithError("Skipping: CPU does not support AVX512F");        \
      return;                                                               \
    }                                                                       \
    const size_t n_elem = buf_size / sizeof(float);                         \
    for (auto _ : state) {                                                  \
      fp32_rz_to_u8_avx512<UC, true>(fp32_buf, uint8_buf, n_elem);          \
    }                                                                       \
    check_result(                                                           \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result); \
    state.counters["elem/s"] = benchmark::Counter(                          \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);  \
  }

FP32_RZ_TO_U8_SATURATE_AVX512(2)
FP32_RZ_TO_U8_SATURATE_AVX512(4)
FP32_RZ_TO_U8_SATURATE_AVX512(6)
FP32_RZ_TO_U8_SATURATE_AVX512(8)

#define FP32_RZ_TO_U8_NARROW_AVX512(UC)                                     \
  BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_narrow_##UC##_avx512)(              \
      benchmark::State & state) {                                           \
    if (!Cpu.avx512f) {                                                     \
      state.SkipWithError("Skipping: CPU does not support AVX512F");        \
      return;                                                               \
    }                                                                       \
    const size_t n_elem = buf_size / sizeof(float);                         \
    for (auto _ : state) {                                                  \
      fp32_rz_to_u8_avx512<UC, false>(fp32_buf, uint8_buf, n_elem);         \
    }                                                                       \
    check_result(                                                           \
        state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result); \
    state.counters["elem/s"] = benchmark::Counter(                          \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);  \
  }

FP32_RZ_TO_U8_NARROW_AVX512(2)
FP32_RZ_TO_U8_NARROW_AVX512(4)
FP32_RZ_TO_U8_NARROW_AVX512(6)
FP32_RZ_TO_U8_NARROW_AVX512(8)

template <int unroll>
__attribute__((target("avx512f"))) __attribute__((noinline)) void
u8_to_fp32_avx512(
    const uint8_t* __restrict__ uint8_buf,
    float* __restrict__ fp32_buf,
    size_t n_elem) {
  const size_t vl = 16;
  size_t i;

#define PROCESS_VECTOR(idx)                                        \
  __m128i i##idx##_0 = _mm_loadu_si128(                            \
      reinterpret_cast<const __m128i*>(uint8_buf + i + idx * vl)); \
  __m512i i32_##idx = _mm512_cvtepu8_epi32(i##idx##_0);            \
  __m512 f##idx = _mm512_cvtepi32_ps(i32_##idx);                   \
  _mm512_storeu_ps(fp32_buf + i + idx * vl, f##idx);

  if constexpr (unroll == 8) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
      PROCESS_VECTOR(6)
      PROCESS_VECTOR(7)
    }
  } else if constexpr (unroll == 6) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
      PROCESS_VECTOR(4)
      PROCESS_VECTOR(5)
    }
  } else if constexpr (unroll == 4) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
      PROCESS_VECTOR(2)
      PROCESS_VECTOR(3)
    }
  } else if constexpr (unroll == 2) {
    for (i = 0; i < n_elem; i += unroll * vl) {
      PROCESS_VECTOR(0)
      PROCESS_VECTOR(1)
    }
  }

#undef PROCESS_VECTOR
}

#define U8_TO_FP32_AVX512(UC)                                              \
  BENCHMARK_F(                                                             \
      FPTypeConv, u8_to_fp32_##UC##_avx512)(benchmark::State & state) {    \
    if (!Cpu.avx512f) {                                                    \
      state.SkipWithError("Skipping: CPU does not support AVX512F");       \
      return;                                                              \
    }                                                                      \
    const size_t n_elem = buf_size / sizeof(float);                        \
    for (auto _ : state) {                                                 \
      u8_to_fp32_avx512<UC>(uint8_buf, fp32_buf, n_elem);                  \
    }                                                                      \
    check_result(                                                          \
        state,                                                             \
        reinterpret_cast<uint8_t*>(fp32_buf),                              \
        buf_size,                                                          \
        crc32_uint8_to_fp32_validation_result);                            \
    state.counters["elem/s"] = benchmark::Counter(                         \
        double(n_elem) * state.iterations(), benchmark::Counter::kIsRate); \
  }

U8_TO_FP32_AVX512(2)
U8_TO_FP32_AVX512(4)
U8_TO_FP32_AVX512(6)
U8_TO_FP32_AVX512(8)

#endif

static __attribute__((noinline)) uint16_t fp32_to_fp16_scalar(float f) {
  uint32_t x = std::bit_cast<uint32_t>(f);

  uint32_t sign = (x >> 31) & 0x1;
  uint32_t exp = (x >> 23) & 0xFF;
  uint32_t mant = x & 0x7FFFFF;

  uint16_t h;

  if (exp == 0xFF) {
    // Inf or NaN
    if (mant == 0) {
      // Infinity
      h = (sign << 15) | (0x1F << 10);
    } else {
      // NaN → quiet NaN, preserve mantissa bits
      h = (sign << 15) | (0x1F << 10) | ((mant >> 13) & 0x3FF);
      // Ensure mantissa is non-zero (quiet NaN)
      if ((h & 0x3FF) == 0)
        h |= 0x200; // Set MSB of mantissa
    }
  } else if (exp > 112) { // 112 = 127 - 15, normal FP16 range
    // Normalized number: FP32 exp in [113, 254] → FP16 exp in [1, 30]

    // Re-bias exponent from FP32 (bias=127) to FP16 (bias=15)
    uint32_t new_exp = exp - 127 + 15;

    if (new_exp >= 0x1F) {
      // Overflow → Inf
      h = (sign << 15) | (0x1F << 10);
    } else {
      // Round-to-nearest-even: drop 13 LSBs from 23-bit mantissa to get 10-bit
      // Round bit is at position 12, sticky bits are [11:0]
      uint32_t round_bit = (mant >> 12) & 1;
      uint32_t sticky_bits = (mant & 0xFFF) != 0;
      uint32_t lsb = (mant >> 13) & 1;

      // Round up if: round_bit && (sticky_bits || lsb)
      // This implements tie-to-even: ties round to even LSB
      uint32_t new_mant = (mant >> 13) + (round_bit & (sticky_bits | lsb));

      // Check for mantissa overflow after rounding
      if (new_mant & 0x400) {
        new_exp += 1;
        new_mant = 0;
      }

      if (new_exp >= 0x1F) {
        // Overflow after rounding → Inf
        h = (sign << 15) | (0x1F << 10);
      } else {
        h = (sign << 15) | (new_exp << 10) | (new_mant & 0x3FF);
      }
    }
  } else if (exp >= 103) { // 103 = 127 - 24, subnormal FP16 range
    // Subnormal FP16: FP32 exp in [103, 112] → FP16 exp = 0
    // Add implicit leading 1 and shift right to denormalize
    uint32_t shift = 113 - exp; // shift amount: 1 to 10
    uint32_t mant_with_leading_one = mant | 0x800000;

    // Round-to-nearest-even during the shift
    // Round bit is at position (shift - 1)
    uint32_t round_bit = (mant_with_leading_one >> (shift - 1)) & 1;
    uint32_t sticky_bits = (shift > 1) &&
        ((mant_with_leading_one & ((1u << (shift - 1)) - 1)) != 0);
    uint32_t shifted_mant = mant_with_leading_one >> shift;
    uint32_t lsb = shifted_mant & 1;

    // Apply RNE
    uint32_t new_mant = shifted_mant + (round_bit & (sticky_bits | lsb));

    h = (sign << 15) | (new_mant & 0x3FF);
  } else {
    // Underflow: too small → zero
    h = (sign << 15);
  }

  return h;
}

BENCHMARK_F(FPTypeConv, fp32_to_fp16_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      half_buf[i] = fp32_to_fp16_scalar(fp32_buf[i]);
    }
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_fp16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

static __attribute__((noinline)) float fp16_to_fp32_scalar(uint16_t h) {
  uint32_t sign = (h >> 15) & 0x1;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;

  uint32_t fp32_bits;

  if (exp == 0x1F) {
    // Inf or NaN
    fp32_bits = (sign << 31) | (0xFF << 23) | (mant << 13);
  } else if (exp == 0) {
    if (mant == 0) {
      // Zero
      fp32_bits = (sign << 31);
    } else {
      // Denormal: normalize the mantissa
      // Find leading 1 by counting leading zeros in mantissa
      uint32_t shift = __builtin_clz(mant) - (32 - 10 - 1);
      // Normalize mantissa and adjust exponent
      mant = (mant << (shift + 1)) & 0x3FF;
      exp = 127 - 15 + 1 - shift;
      fp32_bits = (sign << 31) | (exp << 23) | (mant << 13);
    }
  } else {
    // Normal number: rebias exponent from 15 to 127
    exp = exp - 15 + 127;
    fp32_bits = (sign << 31) | (exp << 23) | (mant << 13);
  }

  return std::bit_cast<float>(fp32_bits);
}

BENCHMARK_F(FPTypeConv, fp16_to_fp32_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      fp32_buf[i] = fp16_to_fp32_scalar(half_buf[i]);
    }
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_fp16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

static __attribute__((noinline)) uint16_t fp32_to_bf16_scalar(float f) {
  uint32_t x = std::bit_cast<uint32_t>(f);

  // Extract high and low parts
  uint32_t hi = x >> 16;
  uint32_t lo = x & 0xFFFF;

  // ---- Handle NaN: preserve payload, force quiet bit ----
  // FP32 NaN: exponent=0xFF and mantissa!=0
  if ((x & 0x7F800000) == 0x7F800000 && (x & 0x007FFFFF)) {
    // convert: preserve top 7 bits of mantissa
    hi |= 0x0040; // ensure quiet NaN in BF16 (mantissa MSB)
    return (uint16_t)hi;
  }

  // ---- Round-to-nearest-even ----
  // Round up if lo > 0x8000, or if lo == 0x8000 and hi is odd (tie → even)
  // This is equivalent to: lo > (0x8000 - (hi & 1))
  // When hi is even: round up if lo > 0x8000
  // When hi is odd:  round up if lo >= 0x8000

  uint32_t rnd = 0x8000 - (hi & 1); // 0x8000 if even, 0x7FFF if odd
  hi += (lo > rnd) ? 1 : 0;

  return (uint16_t)hi;
}

BENCHMARK_F(FPTypeConv, fp32_to_bf16_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      half_buf[i] = fp32_to_bf16_scalar(fp32_buf[i]);
    }
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_bf16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

static __attribute__((noinline)) float bf16_to_fp32_scalar(uint16_t bf16) {
  uint32_t x = static_cast<uint32_t>(bf16) << 16;
  return std::bit_cast<float>(x);
}

BENCHMARK_F(FPTypeConv, bf16_to_fp32_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      fp32_buf[i] = bf16_to_fp32_scalar(half_buf[i]);
    }
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_bf16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

template <bool saturate>
static __attribute__((noinline)) uint8_t fp32_rz_to_u8_scalar(float f) {
  int32_t rounded = static_cast<int32_t>(f);

  if constexpr (saturate) {
    if (rounded > 255) {
      return 255;
    } else if (rounded < 0) {
      return 0;
    }
  }

  return static_cast<uint8_t>(rounded);
}

BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_saturate_scalar)(
    benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      uint8_buf[i] = fp32_rz_to_u8_scalar<true>(fp32_buf[i]);
    }
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK_F(FPTypeConv, fp32_rz_to_u8_narrow_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      uint8_buf[i] = fp32_rz_to_u8_scalar<false>(fp32_buf[i]);
    }
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

static __attribute__((noinline)) float u8_to_fp32_scalar(uint8_t i8) {
  return static_cast<float>(i8);
}

BENCHMARK_F(FPTypeConv, u8_to_fp32_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    for (i = 0; i < n_elem; i += 1) {
      fp32_buf[i] = u8_to_fp32_scalar(uint8_buf[i]);
    }
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_uint8_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK_MAIN();
