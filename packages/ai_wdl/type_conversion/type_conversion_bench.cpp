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
  static const size_t buf_size = 16 * 1024; // 16KB
  static const size_t alignment = 4096; // 4KB-aligned
  static const uint32_t crc32_fp32_to_fp16_validation_result = 2156484379;
  static const uint32_t crc32_fp16_to_fp32_validation_result = 2206894496;
  static const uint32_t crc32_fp32_to_bf16_validation_result = 3316124917;
  static const uint32_t crc32_bf16_to_fp32_validation_result = 3095326091;
  static const uint32_t crc32_fp32_to_uint8_validation_result = 3015812566;
  static const uint32_t crc32_uint8_to_fp32_validation_result = 2455075569;

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

__attribute__((target("fp16"))) __attribute__((noinline)) void
fp32_to_fp16_neon(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  size_t i;
  const size_t vl = 4;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    float32x4_t f0 = vld1q_f32(fp32_buf + i + 0 * vl);
    float32x4_t f1 = vld1q_f32(fp32_buf + i + 1 * vl);
    float32x4_t f2 = vld1q_f32(fp32_buf + i + 2 * vl);
    float32x4_t f3 = vld1q_f32(fp32_buf + i + 3 * vl);

    float16x4_t h0 = vcvt_f16_f32(f0);
    float16x4_t h1 = vcvt_f16_f32(f1);
    float16x4_t h2 = vcvt_f16_f32(f2);
    float16x4_t h3 = vcvt_f16_f32(f3);

    float16x8_t h01_packed = vcombine_f16(h0, h1);
    float16x8_t h23_packed = vcombine_f16(h2, h3);
    vst1q_f16(reinterpret_cast<float16_t*>(half_buf) + i + 0 * vl, h01_packed);
    vst1q_f16(reinterpret_cast<float16_t*>(half_buf) + i + 2 * vl, h23_packed);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_fp16_neon)(benchmark::State& state) {
  if (!Cpu.fp16) {
    state.SkipWithError("Skipping: CPU does not support FEAT_FP16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_fp16_neon(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_fp16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("fp16"))) __attribute__((noinline)) void
fp16_to_fp32_neon(const float16_t* half_buf, float* fp32_buf, size_t n_elem) {
  size_t i;
  const size_t vhl = 8;
  const size_t vwl = vhl / 2;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vwl) {
    float16x8_t h0 = vld1q_f16(half_buf + i + 0 * vhl);
    float16x8_t h1 = vld1q_f16(half_buf + i + 1 * vhl);

    float32x4_t f00 = vcvt_f32_f16(vget_low_f16(h0));
    float32x4_t f01 = vcvt_high_f32_f16(h0);
    float32x4_t f10 = vcvt_f32_f16(vget_low_f16(h1));
    float32x4_t f11 = vcvt_high_f32_f16(h1);

    vst1q_f32(fp32_buf + i + 0 * vwl, f00);
    vst1q_f32(fp32_buf + i + 1 * vwl, f01);
    vst1q_f32(fp32_buf + i + 2 * vwl, f10);
    vst1q_f32(fp32_buf + i + 3 * vwl, f11);
  }
}

BENCHMARK_F(FPTypeConv, fp16_to_fp32_neon)(benchmark::State& state) {
  if (!Cpu.fp16) {
    state.SkipWithError("Skipping: CPU does not support FEAT_FP16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp16_to_fp32_neon(reinterpret_cast<float16_t*>(half_buf), fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_fp16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("sve"))) __attribute__((noinline)) void
fp32_to_fp16_sve(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  size_t i;
  const size_t vl = svcntw();
  const size_t simd_count = 4;
  svbool_t pg = svptrue_b16();
  for (i = 0; i < n_elem; i += simd_count * vl) {
    svfloat32_t f0 = svld1_f32(pg, fp32_buf + i + 0 * vl);
    svfloat32_t f1 = svld1_f32(pg, fp32_buf + i + 1 * vl);
    svfloat32_t f2 = svld1_f32(pg, fp32_buf + i + 2 * vl);
    svfloat32_t f3 = svld1_f32(pg, fp32_buf + i + 3 * vl);
    svfloat16_t h0 = svcvt_f16_f32_x(pg, f0);
    svfloat16_t h1 = svcvt_f16_f32_x(pg, f1);
    svfloat16_t h2 = svcvt_f16_f32_x(pg, f2);
    svfloat16_t h3 = svcvt_f16_f32_x(pg, f3);
    svfloat16_t h01_packed = svuzp1_f16(h0, h1);
    svfloat16_t h23_packed = svuzp1_f16(h2, h3);
    svst1_f16(
        pg, reinterpret_cast<float16_t*>(half_buf) + i + 0 * vl, h01_packed);
    svst1_f16(
        pg, reinterpret_cast<float16_t*>(half_buf) + i + 2 * vl, h23_packed);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_fp16_sve)(benchmark::State& state) {
  if (!Cpu.sve) {
    state.SkipWithError("Skipping: CPU does not support SVE");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_fp16_sve(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_fp16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("sve"))) __attribute__((noinline)) void
fp16_to_fp32_sve(const float16_t* half_buf, float* fp32_buf, size_t n_elem) {
  size_t i;
  const size_t vhl = svcnth();
  const size_t vwl = vhl / 2;
  const size_t simd_count = 4;
  svbool_t pg = svptrue_b32();
  for (i = 0; i < n_elem; i += simd_count * vwl) {
    svfloat16_t h0 = svreinterpret_f16_s32(
        svld1sh_s32(pg, (int16_t*)half_buf + i + 0 * vwl));
    svfloat16_t h1 = svreinterpret_f16_s32(
        svld1sh_s32(pg, (int16_t*)half_buf + i + 1 * vwl));
    svfloat16_t h2 = svreinterpret_f16_s32(
        svld1sh_s32(pg, (int16_t*)half_buf + i + 2 * vwl));
    svfloat16_t h3 = svreinterpret_f16_s32(
        svld1sh_s32(pg, (int16_t*)half_buf + i + 3 * vwl));

    svfloat32_t f0 = svcvt_f32_f16_x(pg, h0);
    svfloat32_t f1 = svcvt_f32_f16_x(pg, h1);
    svfloat32_t f2 = svcvt_f32_f16_x(pg, h2);
    svfloat32_t f3 = svcvt_f32_f16_x(pg, h3);

    svst1_f32(pg, fp32_buf + i + 0 * vwl, f0);
    svst1_f32(pg, fp32_buf + i + 1 * vwl, f1);
    svst1_f32(pg, fp32_buf + i + 2 * vwl, f2);
    svst1_f32(pg, fp32_buf + i + 3 * vwl, f3);
  }
}

BENCHMARK_F(FPTypeConv, fp16_to_fp32_sve)(benchmark::State& state) {
  if (!Cpu.sve) {
    state.SkipWithError("Skipping: CPU does not support SVE");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp16_to_fp32_sve(reinterpret_cast<float16_t*>(half_buf), fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_fp16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("bf16"))) __attribute__((noinline)) void
fp32_to_bf16_neon(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  size_t i;
  const size_t vl = 4;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    float32x4_t f0 = vld1q_f32(fp32_buf + i + 0 * vl);
    float32x4_t f1 = vld1q_f32(fp32_buf + i + 1 * vl);
    float32x4_t f2 = vld1q_f32(fp32_buf + i + 2 * vl);
    float32x4_t f3 = vld1q_f32(fp32_buf + i + 3 * vl);

    bfloat16x4_t h0 = vcvt_bf16_f32(f0);
    bfloat16x4_t h1 = vcvt_bf16_f32(f1);
    bfloat16x4_t h2 = vcvt_bf16_f32(f2);
    bfloat16x4_t h3 = vcvt_bf16_f32(f3);

    bfloat16x8_t h01_packed = vcombine_bf16(h0, h1);
    bfloat16x8_t h23_packed = vcombine_bf16(h2, h3);

    vst1q_bf16(
        reinterpret_cast<bfloat16_t*>(half_buf) + i + 0 * vl, h01_packed);
    vst1q_bf16(
        reinterpret_cast<bfloat16_t*>(half_buf) + i + 2 * vl, h23_packed);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_bf16_neon)(benchmark::State& state) {
  if (!Cpu.bf16) {
    state.SkipWithError("Skipping: CPU does not support FEAT_BF16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_bf16_neon(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_bf16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("bf16"))) __attribute__((noinline)) void
bf16_to_fp32_neon(const bfloat16_t* half_buf, float* fp32_buf, size_t n_elem) {
  size_t i;
  const size_t vhl = 8;
  const size_t vwl = vhl / 2;
  const size_t simd_count = 4;
  uint16x8_t hz = vdupq_n_u16(0);
  // bf16 to fp32 is as simple as a left shift by 16. However, it is faster to
  // use zip1/zip2 instructions instead of sshl/ushl due to higher reciprocal
  // throughput. For example, zip1/zip2's reciprocal throughput is 4 while
  // sshl/ushl's throughput is 2 on Arm Neoverse V2.
  for (i = 0; i < n_elem; i += simd_count * vwl) {
    uint16x8_t h0 = vreinterpretq_u16_bf16(vld1q_bf16(half_buf + i + 0 * vhl));
    uint16x8_t h1 = vreinterpretq_u16_bf16(vld1q_bf16(half_buf + i + 1 * vhl));

    float32x4_t f00 = vreinterpretq_f32_u16(vzip1q_u16(hz, h0));
    float32x4_t f01 = vreinterpretq_f32_u16(vzip2q_u16(hz, h0));
    float32x4_t f10 = vreinterpretq_f32_u16(vzip1q_u16(hz, h1));
    float32x4_t f11 = vreinterpretq_f32_u16(vzip2q_u16(hz, h1));

    vst1q_f32(fp32_buf + i + 0 * vwl, f00);
    vst1q_f32(fp32_buf + i + 1 * vwl, f01);
    vst1q_f32(fp32_buf + i + 2 * vwl, f10);
    vst1q_f32(fp32_buf + i + 3 * vwl, f11);
  }
}

BENCHMARK_F(FPTypeConv, bf16_to_fp32_neon)(benchmark::State& state) {
  if (!Cpu.bf16) {
    state.SkipWithError("Skipping: CPU does not support FEAT_BF16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    bf16_to_fp32_neon(
        reinterpret_cast<bfloat16_t*>(half_buf), fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_bf16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("sve, bf16"))) __attribute__((noinline)) void
fp32_to_bf16_sve(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  const size_t vl = svcntw();
  size_t i;
  const size_t simd_count = 4;
  svbool_t pg = svptrue_b16();
  for (i = 0; i < n_elem; i += simd_count * vl) {
    svfloat32_t f0 = svld1_f32(pg, fp32_buf + i + 0 * vl);
    svfloat32_t f1 = svld1_f32(pg, fp32_buf + i + 1 * vl);
    svfloat32_t f2 = svld1_f32(pg, fp32_buf + i + 2 * vl);
    svfloat32_t f3 = svld1_f32(pg, fp32_buf + i + 3 * vl);

    svbfloat16_t h0 = svcvt_bf16_f32_x(pg, f0);
    svbfloat16_t h1 = svcvt_bf16_f32_x(pg, f1);
    svbfloat16_t h2 = svcvt_bf16_f32_x(pg, f2);
    svbfloat16_t h3 = svcvt_bf16_f32_x(pg, f3);

    svbfloat16_t h01_packed = svuzp1_bf16(h0, h1);
    svbfloat16_t h23_packed = svuzp1_bf16(h2, h3);

    svst1_bf16(
        pg, reinterpret_cast<bfloat16_t*>(half_buf) + i + 0 * vl, h01_packed);
    svst1_bf16(
        pg, reinterpret_cast<bfloat16_t*>(half_buf) + i + 2 * vl, h23_packed);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_bf16_sve)(benchmark::State& state) {
  if (!Cpu.sve || !Cpu.bf16) {
    state.SkipWithError("Skipping: CPU does not support SVE+FEAT_BF16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_bf16_sve(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_bf16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("sve, bf16"))) __attribute__((noinline)) void
bf16_to_fp32_sve(const bfloat16_t* half_buf, float* fp32_buf, size_t n_elem) {
  size_t i;
  const size_t vhl = svcnth();
  const size_t vwl = vhl / 2;
  const size_t simd_count = 4;
  svbool_t pg = svptrue_b16();
  svbfloat16_t hz = svreinterpret_bf16_u16(svdup_u16(0));
  // bf16 to fp32 is as simple as a left-shift by 16, which can be more
  // efficiently done through sve zip1/zip2 instructions.
  // N.B.: sve does not support left-shift and widening, so if zip1/zip2
  //       is not used, we would have to do unpacking first and then left
  //.      shift within each 32-bit element, slower than zip1/zip2.
  for (i = 0; i < n_elem; i += simd_count * vwl) {
    svbfloat16_t h0 = svld1_bf16(pg, half_buf + i + 0 * vhl);
    svbfloat16_t h1 = svld1_bf16(pg, half_buf + i + 1 * vhl);

    svfloat32_t f00 = svreinterpret_f32_bf16(svzip1_bf16(hz, h0));
    svfloat32_t f01 = svreinterpret_f32_bf16(svzip2_bf16(hz, h0));
    svfloat32_t f10 = svreinterpret_f32_bf16(svzip1_bf16(hz, h1));
    svfloat32_t f11 = svreinterpret_f32_bf16(svzip2_bf16(hz, h1));

    svst1_f32(pg, fp32_buf + i + 0 * vwl, f00);
    svst1_f32(pg, fp32_buf + i + 1 * vwl, f01);
    svst1_f32(pg, fp32_buf + i + 2 * vwl, f10);
    svst1_f32(pg, fp32_buf + i + 3 * vwl, f11);
  }
}

BENCHMARK_F(FPTypeConv, bf16_to_fp32_sve)(benchmark::State& state) {
  if (!Cpu.sve || !Cpu.bf16) {
    state.SkipWithError("Skipping: CPU does not support SVE+FEAT_BF16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    bf16_to_fp32_sve(reinterpret_cast<bfloat16_t*>(half_buf), fp32_buf, n_elem);
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
__attribute__((noinline)) void
fp32_to_u8_neon(const float* fp32_buf, uint8_t* uint8_buf, size_t n_elem) {
  size_t i;
  const size_t vl = 4;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    float32x4_t f0 = vld1q_f32(fp32_buf + i + 0 * vl);
    float32x4_t f1 = vld1q_f32(fp32_buf + i + 1 * vl);
    float32x4_t f2 = vld1q_f32(fp32_buf + i + 2 * vl);
    float32x4_t f3 = vld1q_f32(fp32_buf + i + 3 * vl);

    // Round to nearest integer
    uint32x4_t i0 = vcvtnq_u32_f32(f0);
    uint32x4_t i1 = vcvtnq_u32_f32(f1);
    uint32x4_t i2 = vcvtnq_u32_f32(f2);
    uint32x4_t i3 = vcvtnq_u32_f32(f3);

    uint16x8_t i01;
    uint16x8_t i23;
    uint8x16_t i0123;
    if constexpr (saturate) {
      // Narrow from uint32 to uint16 with saturation
      i01 = vcombine_u16(vqmovn_u32(i0), vqmovn_u32(i1));
      i23 = vcombine_u16(vqmovn_u32(i2), vqmovn_u32(i3));

      // Narrow from uint16 to uint8 with saturation
      i0123 = vcombine_u8(vqmovn_u16(i01), vqmovn_u16(i23));
    } else {
      // Narrow from uint32 to uint16
      i01 = vcombine_u16(vmovn_u32(i0), vmovn_u32(i1));
      i23 = vcombine_u16(vmovn_u32(i2), vmovn_u32(i3));

      // Narrow from uint16 to uint8
      i0123 = vcombine_u8(vmovn_u16(i01), vmovn_u16(i23));
    }

    vst1q_u8(uint8_buf + i, i0123);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_saturate_neon)(benchmark::State& state) {
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_neon<true>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_narrow_neon)(benchmark::State& state) {
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_neon<false>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((noinline)) void
u8_to_fp32_neon(const uint8_t* uint8_buf, float* fp32_buf, size_t n_elem) {
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

  for (i = 0; i < n_elem; i += vl) {
    uint8x16_t i0 = vld1q_u8(uint8_buf + i);

    uint32x4_t i0_0 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx_0));
    uint32x4_t i0_1 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx_1));
    uint32x4_t i0_2 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx_2));
    uint32x4_t i0_3 = vreinterpretq_u32_u8(vqtbl1q_u8(i0, idx_3));

    // Convert uint32 to float32
    float32x4_t f0 = vcvtq_f32_u32(i0_0);
    float32x4_t f1 = vcvtq_f32_u32(i0_1);
    float32x4_t f2 = vcvtq_f32_u32(i0_2);
    float32x4_t f3 = vcvtq_f32_u32(i0_3);

    // Store results - 4 writes
    vst1q_f32(fp32_buf + i + 0, f0);
    vst1q_f32(fp32_buf + i + 4, f1);
    vst1q_f32(fp32_buf + i + 8, f2);
    vst1q_f32(fp32_buf + i + 12, f3);
  }
}

BENCHMARK_F(FPTypeConv, u8_to_fp32_neon)(benchmark::State& state) {
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    u8_to_fp32_neon(uint8_buf, fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_uint8_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

template <bool saturate>
__attribute__((target("sve2"))) __attribute__((noinline)) void
fp32_to_u8_sve(const float* fp32_buf, uint8_t* uint8_buf, size_t n_elem) {
  size_t i;
  const size_t vl = svcntw();
  const size_t simd_count = 4;
  svbool_t pg = svptrue_b32();
  for (i = 0; i < n_elem; i += simd_count * vl) {
    svfloat32_t f0 = svld1_f32(pg, fp32_buf + i + 0 * vl);
    svfloat32_t f1 = svld1_f32(pg, fp32_buf + i + 1 * vl);
    svfloat32_t f2 = svld1_f32(pg, fp32_buf + i + 2 * vl);
    svfloat32_t f3 = svld1_f32(pg, fp32_buf + i + 3 * vl);

    // Round to nearest integral, ties to even (banker's rounding)
    // svrintn rounds to nearest as float, then svcvt truncates the
    // already-rounded value.
    // N.B.: SVE does not have an instruction equivalent to neon fcvtnu
    //       so we have to round to nearest integral and then convert
    //       to uint32.
    svfloat32_t r0 = svrintn_f32_x(pg, f0);
    svfloat32_t r1 = svrintn_f32_x(pg, f1);
    svfloat32_t r2 = svrintn_f32_x(pg, f2);
    svfloat32_t r3 = svrintn_f32_x(pg, f3);

    // Convert float to uint32 (truncate, but already rounded)
    svuint32_t i0 = svcvt_u32_f32_x(pg, r0);
    svuint32_t i1 = svcvt_u32_f32_x(pg, r1);
    svuint32_t i2 = svcvt_u32_f32_x(pg, r2);
    svuint32_t i3 = svcvt_u32_f32_x(pg, r3);

    if constexpr (saturate) {
      i0 = svmin_u32_x(pg, i0, svdup_u32(255));
      i1 = svmin_u32_x(pg, i1, svdup_u32(255));
      i2 = svmin_u32_x(pg, i2, svdup_u32(255));
      i3 = svmin_u32_x(pg, i3, svdup_u32(255));
    }

    svst1b_u32(pg, uint8_buf + i + 0 * vl, i0);
    svst1b_u32(pg, uint8_buf + i + 1 * vl, i1);
    svst1b_u32(pg, uint8_buf + i + 2 * vl, i2);
    svst1b_u32(pg, uint8_buf + i + 3 * vl, i3);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_saturate_sve)(benchmark::State& state) {
  if (!Cpu.sve) {
    state.SkipWithError("Skipping: CPU does not support SVE");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_sve<true>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_narrow_sve)(benchmark::State& state) {
  if (!Cpu.sve) {
    state.SkipWithError("Skipping: CPU does not support SVE");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_sve<false>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("sve"))) __attribute__((noinline)) void
uint8_to_fp32_sve(const uint8_t* uint8_buf, float* fp32_buf, size_t n_elem) {
  size_t i;
  const size_t simd_count = 4;
  const size_t vl_s32 = svcntw();
  svbool_t pg = svptrue_b32();
  for (i = 0; i < n_elem; i += simd_count * vl_s32) {
    // Load uint8 and widen directly to uint32 in a single instruction
    svuint32_t i0 = svld1ub_u32(pg, uint8_buf + i + 0 * vl_s32);
    svuint32_t i1 = svld1ub_u32(pg, uint8_buf + i + 1 * vl_s32);
    svuint32_t i2 = svld1ub_u32(pg, uint8_buf + i + 2 * vl_s32);
    svuint32_t i3 = svld1ub_u32(pg, uint8_buf + i + 3 * vl_s32);

    // Convert uint32 to float32
    svfloat32_t f0 = svcvt_f32_u32_x(pg, i0);
    svfloat32_t f1 = svcvt_f32_u32_x(pg, i1);
    svfloat32_t f2 = svcvt_f32_u32_x(pg, i2);
    svfloat32_t f3 = svcvt_f32_u32_x(pg, i3);

    // Store results - 4 writes
    svst1_f32(pg, fp32_buf + i + 0 * vl_s32, f0);
    svst1_f32(pg, fp32_buf + i + 1 * vl_s32, f1);
    svst1_f32(pg, fp32_buf + i + 2 * vl_s32, f2);
    svst1_f32(pg, fp32_buf + i + 3 * vl_s32, f3);
  }
}

BENCHMARK_F(FPTypeConv, uint8_to_fp32_sve)(benchmark::State& state) {
  if (!Cpu.sve) {
    state.SkipWithError("Skipping: CPU does not support SVE");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    uint8_to_fp32_sve(uint8_buf, fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_uint8_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

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

__attribute__((target("avx512f"))) __attribute__((noinline)) void
fp32_to_fp16_avx512(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    __m512 f0 = _mm512_loadu_ps(fp32_buf + i + 0 * vl);
    __m512 f1 = _mm512_loadu_ps(fp32_buf + i + 1 * vl);
    __m512 f2 = _mm512_loadu_ps(fp32_buf + i + 2 * vl);
    __m512 f3 = _mm512_loadu_ps(fp32_buf + i + 3 * vl);

    __m256i h0 = _mm512_cvtps_ph(f0, 0);
    __m256i h1 = _mm512_cvtps_ph(f1, 0);
    __m256i h2 = _mm512_cvtps_ph(f2, 0);
    __m256i h3 = _mm512_cvtps_ph(f3, 0);

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 0 * vl]), h0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 1 * vl]), h1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 2 * vl]), h2);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 3 * vl]), h3);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_fp16_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_fp16_avx512(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_fp16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("avx512f"))) __attribute__((noinline)) void
fp16_to_fp32_avx512(const uint16_t* half_buf, float* fp32_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    __m256i h0 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 0 * vl]));
    __m256i h1 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 1 * vl]));
    __m256i h2 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 2 * vl]));
    __m256i h3 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 3 * vl]));

    __m512 f0 = _mm512_cvtph_ps(h0);
    __m512 f1 = _mm512_cvtph_ps(h1);
    __m512 f2 = _mm512_cvtph_ps(h2);
    __m512 f3 = _mm512_cvtph_ps(h3);

    _mm512_storeu_ps(fp32_buf + i + 0 * vl, f0);
    _mm512_storeu_ps(fp32_buf + i + 1 * vl, f1);
    _mm512_storeu_ps(fp32_buf + i + 2 * vl, f2);
    _mm512_storeu_ps(fp32_buf + i + 3 * vl, f3);
  }
}

BENCHMARK_F(FPTypeConv, fp16_to_fp32_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp16_to_fp32_avx512(half_buf, fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_fp16_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("avx512f, avx512bf16"))) __attribute__((noinline)) void
fp32_to_bf16_avx512(const float* fp32_buf, uint16_t* half_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    __m512 f0 = _mm512_loadu_ps(fp32_buf + i + 0 * vl);
    __m512 f1 = _mm512_loadu_ps(fp32_buf + i + 1 * vl);
    __m512 f2 = _mm512_loadu_ps(fp32_buf + i + 2 * vl);
    __m512 f3 = _mm512_loadu_ps(fp32_buf + i + 3 * vl);

    __m256i h0 = _mm512_cvtneps_pbh(f0);
    __m256i h1 = _mm512_cvtneps_pbh(f1);
    __m256i h2 = _mm512_cvtneps_pbh(f2);
    __m256i h3 = _mm512_cvtneps_pbh(f3);

    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 0 * vl]), h0);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 1 * vl]), h1);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 2 * vl]), h2);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(&half_buf[i + 3 * vl]), h3);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_bf16_avx512)(benchmark::State& state) {
  if (!Cpu.avx512bf16) {
    state.SkipWithError("Skipping: CPU does not support AVX512F+AVX512BF16");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_bf16_avx512(fp32_buf, half_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(half_buf),
      buf_size,
      crc32_fp32_to_bf16_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("avx512f"))) __attribute__((noinline)) void
bf16_to_fp32_avx512(const uint16_t* half_buf, float* fp32_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  __m256i zero = _mm256_setzero_si256();
  // BF16 to FP32 is a simple left shift by 16 bits
  // Use unpack instructions for efficient conversion
  for (i = 0; i < n_elem; i += simd_count * vl) {
    __m256i h0 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 0 * vl]));
    __m256i h1 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 1 * vl]));
    __m256i h2 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 2 * vl]));
    __m256i h3 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(&half_buf[i + 3 * vl]));

    __m512i f0 = _mm512_cvtepu16_epi32(h0);
    __m512i f1 = _mm512_cvtepu16_epi32(h1);
    __m512i f2 = _mm512_cvtepu16_epi32(h2);
    __m512i f3 = _mm512_cvtepu16_epi32(h3);

    f0 = _mm512_slli_epi32(f0, 16);
    f1 = _mm512_slli_epi32(f1, 16);
    f2 = _mm512_slli_epi32(f2, 16);
    f3 = _mm512_slli_epi32(f3, 16);

    _mm512_storeu_ps(fp32_buf + i + 0 * vl, _mm512_castsi512_ps(f0));
    _mm512_storeu_ps(fp32_buf + i + 1 * vl, _mm512_castsi512_ps(f1));
    _mm512_storeu_ps(fp32_buf + i + 2 * vl, _mm512_castsi512_ps(f2));
    _mm512_storeu_ps(fp32_buf + i + 3 * vl, _mm512_castsi512_ps(f3));
  }
}

BENCHMARK_F(FPTypeConv, bf16_to_fp32_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    bf16_to_fp32_avx512(half_buf, fp32_buf, n_elem);
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
__attribute__((target("avx512f, avx512bw"))) __attribute__((noinline)) void
fp32_to_u8_avx512(const float* fp32_buf, uint8_t* uint8_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    __m512 f0 = _mm512_loadu_ps(fp32_buf + i + 0 * vl);
    __m512 f1 = _mm512_loadu_ps(fp32_buf + i + 1 * vl);
    __m512 f2 = _mm512_loadu_ps(fp32_buf + i + 2 * vl);
    __m512 f3 = _mm512_loadu_ps(fp32_buf + i + 3 * vl);

    // Convert float to int32 with rounding to nearest (banker's rounding)
    // This is a single instruction, more efficient than separate round +
    // convert
    __m512i i0 = _mm512_cvt_roundps_epi32(
        f0, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m512i i1 = _mm512_cvt_roundps_epi32(
        f1, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m512i i2 = _mm512_cvt_roundps_epi32(
        f2, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m512i i3 = _mm512_cvt_roundps_epi32(
        f3, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);

    if constexpr (saturate) {
      // Narrow from uint32 to uint16 with saturation (packs interleave)
      __m512i i01_16 = _mm512_packus_epi32(i0, i1);
      __m512i i23_16 = _mm512_packus_epi32(i2, i3);

      // Narrow from uint16 to uint8 with saturation
      __m512i i0123_8 = _mm512_packus_epi16(i01_16, i23_16);

      // After two packs, each dword contains 4 consecutive uint8 values from
      // one source. Dwords are interleaved, so we just need to permute at
      // dword-level to get: [i0[0..15], i1[0..15], i2[0..15], i3[0..15]]
      const __m512i perm_idx = _mm512_setr_epi32(
          0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15);
      i0123_8 = _mm512_permutexvar_epi32(perm_idx, i0123_8);

      _mm512_storeu_si512(reinterpret_cast<__m512i*>(uint8_buf + i), i0123_8);
    } else {
      _mm512_mask_cvtusepi32_storeu_epi8(
          reinterpret_cast<__m512i*>(uint8_buf + i), 0xFFFF, i0);
      _mm512_mask_cvtusepi32_storeu_epi8(
          reinterpret_cast<__m512i*>(uint8_buf + i + 16), 0xFFFF, i1);
      _mm512_mask_cvtusepi32_storeu_epi8(
          reinterpret_cast<__m512i*>(uint8_buf + i + 32), 0xFFFF, i2);
      _mm512_mask_cvtusepi32_storeu_epi8(
          reinterpret_cast<__m512i*>(uint8_buf + i + 48), 0xFFFF, i3);
    }
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_saturate_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_avx512<true>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_narrow_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    fp32_to_u8_avx512<false>(fp32_buf, uint8_buf, n_elem);
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

__attribute__((target("avx512f"))) __attribute__((noinline)) void
u8_to_fp32_avx512(const uint8_t* uint8_buf, float* fp32_buf, size_t n_elem) {
  const size_t vl = 16;
  size_t i;
  const size_t simd_count = 4;
  for (i = 0; i < n_elem; i += simd_count * vl) {
    // Load 4 separate 128-bit chunks (16 uint8 values each)
    // This is faster than loading 512-bit and extracting due to better port
    // distribution
    __m128i i0_0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(uint8_buf + i + 0 * vl));
    __m128i i0_1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(uint8_buf + i + 1 * vl));
    __m128i i0_2 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(uint8_buf + i + 2 * vl));
    __m128i i0_3 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(uint8_buf + i + 3 * vl));

    // Zero-extend uint8 to uint32
    __m512i i32_0 = _mm512_cvtepu8_epi32(i0_0);
    __m512i i32_1 = _mm512_cvtepu8_epi32(i0_1);
    __m512i i32_2 = _mm512_cvtepu8_epi32(i0_2);
    __m512i i32_3 = _mm512_cvtepu8_epi32(i0_3);

    // Convert uint32 to float32
    __m512 f0 = _mm512_cvtepi32_ps(i32_0);
    __m512 f1 = _mm512_cvtepi32_ps(i32_1);
    __m512 f2 = _mm512_cvtepi32_ps(i32_2);
    __m512 f3 = _mm512_cvtepi32_ps(i32_3);

    // Store results
    _mm512_storeu_ps(fp32_buf + i + 0 * vl, f0);
    _mm512_storeu_ps(fp32_buf + i + 1 * vl, f1);
    _mm512_storeu_ps(fp32_buf + i + 2 * vl, f2);
    _mm512_storeu_ps(fp32_buf + i + 3 * vl, f3);
  }
}

BENCHMARK_F(FPTypeConv, u8_to_fp32_avx512)(benchmark::State& state) {
  if (!Cpu.avx512f) {
    state.SkipWithError("Skipping: CPU does not support AVX512F");
    return;
  }

  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
    u8_to_fp32_avx512(uint8_buf, fp32_buf, n_elem);
  }

  check_result(
      state,
      reinterpret_cast<uint8_t*>(fp32_buf),
      buf_size,
      crc32_uint8_to_fp32_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

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
#pragma clang loop unroll(disable) vectorize(disable)
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
#pragma clang loop unroll(disable) vectorize(disable)
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
#pragma clang loop unroll(disable) vectorize(disable)
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
#pragma clang loop unroll(disable) vectorize(disable)
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

static __attribute__((noinline)) uint8_t fp32_to_u8_saturate_scalar(float f) {
  // Round to nearest integer with ties to even (banker's rounding)
  // nearbyintf respects the current rounding mode set by fesetround
  // (FE_TONEAREST)
  int32_t rounded = static_cast<int32_t>(std::nearbyintf(f));

  // Clamp to uint8 range [0, 257]
  if (rounded > 255) {
    return 255;
  } else if (rounded < 0) {
    return 0;
  } else {
    return static_cast<uint8_t>(rounded);
  }
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_saturate_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
#pragma clang loop unroll(disable) vectorize(disable)
    for (i = 0; i < n_elem; i += 1) {
      uint8_buf[i] = fp32_to_u8_saturate_scalar(fp32_buf[i]);
    }
  }

  check_result(
      state, uint8_buf, buf_size, crc32_fp32_to_uint8_validation_result);

  state.counters["elem/s"] = benchmark::Counter(
      double(n_elem) * state.iterations(), benchmark::Counter::kIsRate);
}

static __attribute__((noinline)) uint8_t fp32_to_u8_narrow_scalar(float f) {
  int32_t rounded = static_cast<int32_t>(std::nearbyintf(f));
  return static_cast<uint8_t>(rounded);
}

BENCHMARK_F(FPTypeConv, fp32_to_u8_narrow_scalar)(benchmark::State& state) {
  size_t i;
  const size_t n_elem = buf_size / sizeof(float);
  for (auto _ : state) {
#pragma clang loop unroll(disable) vectorize(disable)
    for (i = 0; i < n_elem; i += 1) {
      uint8_buf[i] = fp32_to_u8_narrow_scalar(fp32_buf[i]);
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
#pragma clang loop unroll(disable) vectorize(disable)
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
