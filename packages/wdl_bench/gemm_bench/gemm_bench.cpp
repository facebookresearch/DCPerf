// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

#if defined(USE_OPENBLAS)
#include <cblas.h>

#elif defined(USE_AOCL)
#include <blis.h>
#include <cblas.h>

#elif defined(USE_ONEDNN)
#include <dnnl.hpp>

#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <mkl.h>
#define USE_MKL 1

using bfloat16 = MKL_BF16;

#elif defined(__aarch64__) || defined(_M_ARM64)
#include <armpl.h>
#include <omp.h>

// Arm Compute Library headers for INT8 and BF16 GEMM
#include <arm_compute/core/Types.h>
#include <arm_compute/function_info/GEMMInfo.h>
#include <arm_compute/runtime/NEON/NEFunctions.h>
#include <arm_compute/runtime/NEON/NEScheduler.h>
#include <arm_compute/runtime/NEON/functions/NEGEMM.h>
#include <arm_compute/runtime/NEON/functions/NEGEMMLowpMatrixMultiplyCore.h>
#include <arm_compute/runtime/Tensor.h>
#define USE_APL_ACL 1

using bfloat16 = uint16_t;

#endif

// Size range for automatic benchmarking
static constexpr int kMinSize = 32;
static constexpr int kMaxSize = 4096;

namespace {

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

template <typename T>
uint32_t compute_matrix_crc32(const std::vector<T>& vec) {
  return crc32(
      reinterpret_cast<const uint8_t*>(vec.data()), vec.size() * sizeof(T));
}

template <bool CHECK_RESULT = false>
void check_result(benchmark::State& state, uint32_t result, uint32_t ref) {
  if constexpr (CHECK_RESULT) {
    if (result != ref) {
      std::string msg = "Skipping: result validation failed (expected: " +
          std::to_string(ref) + ", actual: " + std::to_string(result) + ")";
      state.SkipWithError(msg.c_str());
    }
  }
}

template <typename T>
void fillMatrix(std::vector<T>& vec) {
  for (auto& v : vec) {
    if constexpr (std::is_same_v<T, float>) {
      v = 1.0f;
    } else if constexpr (
        std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>) {
      v = 1;
    }
#if !defined(USE_ONEDNN)
    else if constexpr (std::is_same_v<T, bfloat16>) {
      v = 0x3F80; // 1.0 in bfloat16
    }
#endif
#if defined(__aarch64__)
    else if constexpr (std::is_same_v<T, __fp16>) {
      v = static_cast<__fp16>(1.0f);
    }
#endif
    else {
      constexpr bool type_conversion_supported = std::is_same_v<T, float> ||
          std::is_same_v<T, int8_t> || std::is_same_v<T, uint8_t>
#if !defined(USE_ONEDNN)
          || std::is_same_v<T, bfloat16>
#endif
          ;
#if defined(__aarch64__)
      type_conversion_supported =
          type_conversion_supported || std::is_same_v<T, __fp16>;
#endif
      static_assert(type_conversion_supported, "Unsupported matrix type");
    }
  }
}

template <typename T>
float toFloat(T val) {
  return static_cast<float>(val);
}

template <typename T, bool PRINT_MATRIX = false>
void printMatrix(const std::vector<T>& mat, int M, int N, const char* name) {
  if constexpr (!PRINT_MATRIX) {
    return;
  }
  constexpr int kMaxPrint = 4;
  const int printM = std::min(M, kMaxPrint);
  const int printN = std::min(N, kMaxPrint);

  std::cout << "\n"
            << name << " (showing " << printM << "x" << printN << " of " << M
            << "x" << N << "):\n";
  std::cout << std::fixed << std::setprecision(4);

  for (int i = 0; i < printM; ++i) {
    std::cout << "  [";
    for (int j = 0; j < printN; ++j) {
      if (j > 0)
        std::cout << ", ";
      std::cout << std::setw(10) << toFloat(mat[i * N + j]);
    }
    if (N > kMaxPrint)
      std::cout << ", ...";
    std::cout << "]\n";
  }
  if (M > kMaxPrint)
    std::cout << "  ...\n";
}

#if defined(USE_MKL)

void BM_SGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<float> A(M * K);
  std::vector<float> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  const float alpha = 1.0f;
  const float beta = 0.0f;

  for (auto _ : state) {
    cblas_sgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  // Report FLOPS: 2*M*N*K operations per GEMM
  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C, M, N, "SGEMM Result C");
}

void BM_I8GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // MKL gemm_s8u8s32: A is int8, B is uint8, C is int32
  std::vector<int8_t> A(M * K);
  std::vector<uint8_t> B(K * N);
  std::vector<int32_t> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  std::memset(C.data(), 0, C.size() * sizeof(int32_t));

  const float alpha = 1.0f;
  const float beta = 0.0f;
  const int8_t ao = 0; // A offset
  const int8_t bo = 0; // B offset
  const int32_t co = 0; // C offset

  for (auto _ : state) {
    cblas_gemm_s8u8s32(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        CblasFixOffset,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        ao,
        B.data(),
        N,
        bo,
        beta,
        C.data(),
        N,
        &co);
    benchmark::DoNotOptimize(C.data());
  }

  // INT8 GEMM: 2*M*N*K integer ops
  const double flops = 2.0 * M * N * K;
  state.counters["TOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

void BM_BF16GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // MKL gemm_bf16bf16f32: A and B are bfloat16, C is float32
  std::vector<MKL_BF16> A(M * K);
  std::vector<MKL_BF16> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  std::memset(C.data(), 0, C.size() * sizeof(float));

  const float alpha = 1.0f;
  const float beta = 0.0f;

  for (auto _ : state) {
    cblas_gemm_bf16bf16f32(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

inline MKL_F16 floatToHalf(float f) {
  // Use F16C intrinsic: _MM_FROUND_TO_NEAREST_INT for rounding mode
  return static_cast<MKL_F16>(_cvtss_sh(f, _MM_FROUND_TO_NEAREST_INT));
}

void BM_HGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<MKL_F16> A(M * K);
  std::vector<MKL_F16> B(K * N);
  std::vector<MKL_F16> C(M * N);

  for (auto& v : A) {
    v = floatToHalf(1.0f);
  }
  for (auto& v : B) {
    v = floatToHalf(1.0f);
  }

  std::memset(C.data(), 0, C.size() * sizeof(MKL_F16));

  const MKL_F16 alpha = floatToHalf(1.0f);
  const MKL_F16 beta = floatToHalf(0.0f);

  for (auto _ : state) {
    cblas_hgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C, M, N, "HGEMM Result C");
}

#elif defined(USE_APL_ACL)

void BM_SGEMM_APL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<float> A(M * K);
  std::vector<float> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  const float alpha = 1.0f;
  const float beta = 0.0f;

  for (auto _ : state) {
    cblas_sgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C, M, N, "SGEMM Result C");
}

void BM_I8GEMM_ACL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // ACL uses QASYMM8_SIGNED for signed int8
  std::vector<int8_t> A_data(M * K);
  std::vector<int8_t> B_data(K * N);
  std::vector<int32_t> C_data(M * N);

  fillMatrix(A_data);
  fillMatrix(B_data);

  std::memset(C_data.data(), 0, C_data.size() * sizeof(int32_t));

  // Create tensor infos - ACL uses (width, height) format
  // For row-major: A is MxK, so TensorShape(K, M)
  auto a_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(K, M),
      1,
      arm_compute::DataType::QASYMM8_SIGNED,
      arm_compute::QuantizationInfo(1.0f, 0));

  auto b_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, K),
      1,
      arm_compute::DataType::QASYMM8_SIGNED,
      arm_compute::QuantizationInfo(1.0f, 0));

  // Output as S32 (no requantization, just accumulate)
  auto c_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, M), 1, arm_compute::DataType::S32);

  // Create tensors
  arm_compute::Tensor a_tensor, b_tensor, c_tensor;
  a_tensor.allocator()->init(a_info);
  b_tensor.allocator()->init(b_info);
  c_tensor.allocator()->init(c_info);

  a_tensor.allocator()->allocate();
  b_tensor.allocator()->allocate();
  c_tensor.allocator()->allocate();

  // Copy data to tensors
  std::memcpy(a_tensor.buffer(), A_data.data(), A_data.size() * sizeof(int8_t));
  std::memcpy(b_tensor.buffer(), B_data.data(), B_data.size() * sizeof(int8_t));

  // Configure GEMM
  arm_compute::NEGEMMLowpMatrixMultiplyCore gemm;
  arm_compute::GEMMInfo gemm_info;

  auto status = arm_compute::NEGEMMLowpMatrixMultiplyCore::validate(
      a_tensor.info(), b_tensor.info(), nullptr, c_tensor.info(), gemm_info);

  if (status.error_code() != arm_compute::ErrorCode::OK) {
    state.SkipWithError(
        ("I8GEMM validation failed: " + status.error_description()).c_str());
    return;
  }

  gemm.configure(&a_tensor, &b_tensor, nullptr, &c_tensor, gemm_info);

  for (auto _ : state) {
    gemm.run();
    benchmark::DoNotOptimize(c_tensor.buffer());
  }

  // Copy result back
  std::memcpy(
      C_data.data(), c_tensor.buffer(), C_data.size() * sizeof(int32_t));

  const double ops = 2.0 * M * N * K;
  state.counters["TOPS"] = benchmark::Counter(
      ops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  a_tensor.allocator()->free();
  b_tensor.allocator()->free();
  c_tensor.allocator()->free();
}

void BM_BF16GEMM_ACL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // Prepare bfloat16 input data, float32 output
  std::vector<bfloat16> A_data(M * K);
  std::vector<bfloat16> B_data(K * N);
  std::vector<float> C_data(M * N);

  fillMatrix(A_data);
  fillMatrix(B_data);

  std::memset(C_data.data(), 0, C_data.size() * sizeof(float));

  // Create tensor infos - ACL uses (width, height) format
  // Inputs are BF16, output is F32 for better precision
  auto a_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(K, M), 1, arm_compute::DataType::BFLOAT16);

  auto b_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, K), 1, arm_compute::DataType::BFLOAT16);

  // Output as F32 for accumulation precision (matches MKL's
  // cblas_gemm_bf16bf16f32)
  auto c_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, M), 1, arm_compute::DataType::F32);

  // Create tensors
  arm_compute::Tensor a_tensor, b_tensor, c_tensor;
  a_tensor.allocator()->init(a_info);
  b_tensor.allocator()->init(b_info);
  c_tensor.allocator()->init(c_info);

  a_tensor.allocator()->allocate();
  b_tensor.allocator()->allocate();
  c_tensor.allocator()->allocate();

  // Copy data to tensors
  std::memcpy(
      a_tensor.buffer(), A_data.data(), A_data.size() * sizeof(bfloat16));
  std::memcpy(
      b_tensor.buffer(), B_data.data(), B_data.size() * sizeof(bfloat16));

  // Configure GEMM
  arm_compute::NEGEMM gemm;
  arm_compute::GEMMInfo gemm_info;

  auto status = arm_compute::NEGEMM::validate(
      a_tensor.info(),
      b_tensor.info(),
      nullptr,
      c_tensor.info(),
      1.0f,
      0.0f,
      gemm_info);

  if (status.error_code() != arm_compute::ErrorCode::OK) {
    state.SkipWithError(
        ("BF16GEMM validation failed: " + status.error_description()).c_str());
    return;
  }

  gemm.configure(
      &a_tensor, &b_tensor, nullptr, &c_tensor, 1.0f, 0.0f, gemm_info);

  for (auto _ : state) {
    gemm.run();
    benchmark::DoNotOptimize(c_tensor.buffer());
  }

  // Copy result back
  std::memcpy(C_data.data(), c_tensor.buffer(), C_data.size() * sizeof(float));

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  a_tensor.allocator()->free();
  b_tensor.allocator()->free();
  c_tensor.allocator()->free();
}

void BM_HGEMM_APL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // ARMPL uses __fp16 for half precision
  std::vector<__fp16> A(M * K);
  std::vector<__fp16> B(K * N);
  std::vector<__fp16> C(M * N);

  fillMatrix(A);
  fillMatrix(B);
  std::memset(C.data(), 0, C.size() * sizeof(__fp16));

  const __fp16 alpha = static_cast<__fp16>(1.0f);
  const __fp16 beta = static_cast<__fp16>(0.0f);

  for (auto _ : state) {
    cblas_hgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

void BM_SGEMM_ACL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // Prepare float32 data
  std::vector<float> A_data(M * K);
  std::vector<float> B_data(K * N);
  std::vector<float> C_data(M * N);

  fillMatrix(A_data);
  fillMatrix(B_data);

  std::memset(C_data.data(), 0, C_data.size() * sizeof(float));

  // Create tensor infos - ACL uses (width, height) format
  auto a_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(K, M), 1, arm_compute::DataType::F32);

  auto b_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, K), 1, arm_compute::DataType::F32);

  auto c_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, M), 1, arm_compute::DataType::F32);

  // Create tensors
  arm_compute::Tensor a_tensor, b_tensor, c_tensor;
  a_tensor.allocator()->init(a_info);
  b_tensor.allocator()->init(b_info);
  c_tensor.allocator()->init(c_info);

  a_tensor.allocator()->allocate();
  b_tensor.allocator()->allocate();
  c_tensor.allocator()->allocate();

  // Copy data to tensors
  std::memcpy(a_tensor.buffer(), A_data.data(), A_data.size() * sizeof(float));
  std::memcpy(b_tensor.buffer(), B_data.data(), B_data.size() * sizeof(float));

  // Configure GEMM
  arm_compute::NEGEMM gemm;
  arm_compute::GEMMInfo gemm_info;

  auto status = arm_compute::NEGEMM::validate(
      a_tensor.info(),
      b_tensor.info(),
      nullptr,
      c_tensor.info(),
      1.0f,
      0.0f,
      gemm_info);

  if (status.error_code() != arm_compute::ErrorCode::OK) {
    state.SkipWithError(
        ("SGEMM_ACL validation failed: " + status.error_description()).c_str());
    return;
  }

  gemm.configure(
      &a_tensor, &b_tensor, nullptr, &c_tensor, 1.0f, 0.0f, gemm_info);

  for (auto _ : state) {
    gemm.run();
    benchmark::DoNotOptimize(c_tensor.buffer());
  }

  // Copy result back
  std::memcpy(C_data.data(), c_tensor.buffer(), C_data.size() * sizeof(float));

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C_data, M, N, "SGEMM_ACL Result C");

  a_tensor.allocator()->free();
  b_tensor.allocator()->free();
  c_tensor.allocator()->free();
}

void BM_HGEMM_ACL(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<__fp16> A_data(M * K);
  std::vector<__fp16> B_data(K * N);
  std::vector<__fp16> C_data(M * N);

  // Fill with 1.0 in FP16
  for (auto& v : A_data)
    v = static_cast<__fp16>(1.0f);
  for (auto& v : B_data)
    v = static_cast<__fp16>(1.0f);

  std::memset(C_data.data(), 0, C_data.size() * sizeof(__fp16));

  // Create tensor infos - ACL uses (width, height) format
  auto a_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(K, M), 1, arm_compute::DataType::F16);

  auto b_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, K), 1, arm_compute::DataType::F16);

  // NOTE: ACL's GEMM doesn't support FP32 output, so we use F16
  auto c_info = arm_compute::TensorInfo(
      arm_compute::TensorShape(N, M), 1, arm_compute::DataType::F16);

  // Create tensors
  arm_compute::Tensor a_tensor, b_tensor, c_tensor;
  a_tensor.allocator()->init(a_info);
  b_tensor.allocator()->init(b_info);
  c_tensor.allocator()->init(c_info);

  a_tensor.allocator()->allocate();
  b_tensor.allocator()->allocate();
  c_tensor.allocator()->allocate();

  // Copy data to tensors
  std::memcpy(a_tensor.buffer(), A_data.data(), A_data.size() * sizeof(__fp16));
  std::memcpy(b_tensor.buffer(), B_data.data(), B_data.size() * sizeof(__fp16));

  // Configure GEMM
  arm_compute::NEGEMM gemm;
  arm_compute::GEMMInfo gemm_info;

  auto status = arm_compute::NEGEMM::validate(
      a_tensor.info(),
      b_tensor.info(),
      nullptr,
      c_tensor.info(),
      1.0f,
      0.0f,
      gemm_info);

  if (status.error_code() != arm_compute::ErrorCode::OK) {
    state.SkipWithError(
        ("HGEMM validation failed: " + status.error_description()).c_str());
    return;
  }

  gemm.configure(
      &a_tensor, &b_tensor, nullptr, &c_tensor, 1.0f, 0.0f, gemm_info);

  for (auto _ : state) {
    gemm.run();
    benchmark::DoNotOptimize(c_tensor.buffer());
  }

  // Copy result back
  std::memcpy(C_data.data(), c_tensor.buffer(), C_data.size() * sizeof(__fp16));

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C_data, M, N, "HGEMM Result C");

  a_tensor.allocator()->free();
  b_tensor.allocator()->free();
  c_tensor.allocator()->free();
}

#elif defined(USE_OPENBLAS)

void BM_SGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<float> A(M * K);
  std::vector<float> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  const float alpha = 1.0f;
  const float beta = 0.0f;

  for (auto _ : state) {
    cblas_sgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C, M, N, "SGEMM Result C");
}

void BM_I8GEMM(benchmark::State& state) {
  state.SkipWithError(
      "Not implemented because OpenBLAS doesn't support I8 GEMM");
}

void BM_BF16GEMM(benchmark::State& state) {
  state.SkipWithError(
      "Not implemented because OpenBLAS doesn't support BF16 GEMM");
}

void BM_HGEMM(benchmark::State& state) {
  state.SkipWithError("Not implemented because OpenBLAS doesn't support FP16");
}

#elif defined(USE_ONEDNN)

using namespace dnnl;
struct gemm_dims_t {
  memory::dim m, n, k;
};

inline void write_to_dnnl_memory(void* handle, dnnl::memory& mem) {
  dnnl::engine eng = mem.get_engine();
  size_t size = mem.get_desc().get_size();

  if (!handle) {
    throw std::runtime_error("handle is nullptr.");
  }

  if (eng.get_kind() == dnnl::engine::kind::cpu) {
    uint8_t* dst = static_cast<uint8_t*>(mem.get_data_handle());
    if (!dst)
      throw std::runtime_error("get_data_handle returned nullptr.");
    for (size_t i = 0; i < size; ++i)
      dst[i] = ((uint8_t*)handle)[i];
    return;
  }

  assert(false && "not expected");
}

static void
dnn_run(benchmark::State& state, memory::data_type type, gemm_dims_t dims) {
  bool is_integer =
      (type == memory::data_type::s8 || type == memory::data_type::u8);

  // Create execution dnnl::engine.
  dnnl::engine engine(dnnl::engine::kind::cpu, 0);

  // Create dnnl::stream.
  dnnl::stream engine_stream(engine);

  // Source (A), weights (B), and destination (C) matrix dimensions.
  memory::dims a_dims = {dims.m, dims.k};
  memory::dims b_dims = {dims.k, dims.n};
  memory::dims c_dims = {dims.m, dims.n};

  // Allocate buffers and random-initialize A/B
  std::vector<float> a_data(dims.m * dims.k);
  std::vector<float> b_data(dims.k * dims.n);
  std::vector<float> c_data(dims.m * dims.n);

  for (auto& v : a_data) {
    if (is_integer) {
      v = std::bit_cast<float>(int(1));
    } else {
      v = 1.0f;
    }
  }

  for (auto& v : b_data) {
    if (is_integer) {
      v = std::bit_cast<float>(int(1));
    } else {
      v = 1.0f;
    }
  }

  // Create memory descriptors and memory objects for src, weights, bias, and
  // dst.
  auto a_md = memory::desc(a_dims, type, memory::format_tag::any);
  auto b_md = memory::desc(b_dims, type, memory::format_tag::any);
  auto c_md = memory::desc(c_dims, type, memory::format_tag::any);

  auto a_in_md =
      memory::desc(a_dims, memory::data_type::f32, memory::format_tag::ab);
  auto b_in_md =
      memory::desc(b_dims, memory::data_type::f32, memory::format_tag::ab);

  auto a_in_mem = memory(a_in_md, engine);
  auto b_in_mem = memory(b_in_md, engine);

  // Write data to memory object's handles.
  write_to_dnnl_memory(a_data.data(), a_in_mem);
  write_to_dnnl_memory(b_data.data(), b_in_mem);

  // Create primitive descriptor.
  auto matmul_pd = matmul::primitive_desc(engine, a_md, b_md, c_md);

  // Repack and convert input data.
  auto a_mem = memory(matmul_pd.src_desc(), engine);
  reorder(a_in_mem, a_mem).execute(engine_stream, a_in_mem, a_mem);

  auto b_mem = memory(matmul_pd.weights_desc(), engine);
  reorder(b_in_mem, b_mem).execute(engine_stream, b_in_mem, b_mem);

  auto c_mem = memory(matmul_pd.dst_desc(), engine);

  // Create the primitive.
  auto matmul_prim = matmul(matmul_pd);

  // Primitive arguments.
  std::unordered_map<int, memory> matmul_args;
  matmul_args.insert({DNNL_ARG_SRC, a_mem});
  matmul_args.insert({DNNL_ARG_WEIGHTS, b_mem});
  matmul_args.insert({DNNL_ARG_DST, c_mem});

  for (auto _ : state) {
    matmul_prim.execute(engine_stream, matmul_args);
  }
  engine_stream.wait();

  const double flops = 2.0 * dims.m * dims.n * dims.k;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

void BM_SGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  try {
    dnn_run(state, memory::data_type::f32, {M, N, K});
  } catch (dnnl::error& e) {
    // Catch and report unimplemented cases.
    if (e.status == dnnl_unimplemented) {
      state.SkipWithError(
          "Not implemented because F32 gemm is not supported by oneDNN on this CPU");
    } else {
      state.SkipWithError("Unexpected error");
    }
  }
}

void BM_I8GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  try {
    dnn_run(state, memory::data_type::s8, {M, N, K});
  } catch (dnnl::error& e) {
    // Catch and report unimplemented cases.
    if (e.status == dnnl_unimplemented) {
      state.SkipWithError(
          "Not implemented because I8 gemm is not supported by oneDNN on this CPU");
    } else {
      state.SkipWithError("Unexpected error");
    }
  }
}

void BM_BF16GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  try {
    dnn_run(state, memory::data_type::bf16, {M, N, K});
  } catch (dnnl::error& e) {
    // Catch and report unimplemented cases.
    if (e.status == dnnl_unimplemented) {
      state.SkipWithError(
          "Not implemented because BF16 gemm is not supported by oneDNN on this CPU");
    } else {
      state.SkipWithError("Unexpected error");
    }
  }
}

void BM_HGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  try {
    dnn_run(state, memory::data_type::f16, {M, N, K});
  } catch (dnnl::error& e) {
    // Catch and report unimplemented cases.
    if (e.status == dnnl_unimplemented) {
      state.SkipWithError(
          "Not implemented because F16 gemm is not supported by oneDNN on this CPU");
    } else {
      state.SkipWithError("Unexpected error");
    }
  }
}

#elif defined(USE_AOCL)

// AOCL GEMM API reference:
// https://amd.github.io/aocl-dlp/api/gemm/

void BM_SGEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<float> A(M * K);
  std::vector<float> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  const float alpha = 1.0f;
  const float beta = 0.0f;

  for (auto _ : state) {
    cblas_sgemm(
        CblasRowMajor,
        CblasNoTrans,
        CblasNoTrans,
        M,
        N,
        K,
        alpha,
        A.data(),
        K,
        B.data(),
        N,
        beta,
        C.data(),
        N);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);

  printMatrix(C, M, N, "SGEMM Result C");
}

void BM_I8GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  std::vector<int8_t> A(M * K);
  std::vector<int8_t> B(K * N);
  std::vector<int32_t> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  std::memset(C.data(), 0, C.size() * sizeof(int32_t));

  for (auto _ : state) {
    aocl_gemm_s8s8s32os32(
        'R', // Row major
        'N', // No transpose A
        'N', // No transpose B
        M,
        N,
        K,
        1, // alpha
        A.data(),
        K, // lda
        'N',
        B.data(),
        N, // ldb
        'N',
        0, // beta
        C.data(),
        N, // ldc
        0);
    benchmark::DoNotOptimize(C.data());
  }

  // INT8 GEMM: 2*M*N*K integer ops
  const double flops = 2.0 * M * N * K;
  state.counters["TOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

void BM_BF16GEMM(benchmark::State& state) {
  const int M = state.range(0);
  const int N = state.range(1);
  const int K = state.range(2);

  // bf16bf16f32of32: A and B are bfloat16, C is float32
  std::vector<bfloat16> A(M * K);
  std::vector<bfloat16> B(K * N);
  std::vector<float> C(M * N);

  fillMatrix(A);
  fillMatrix(B);

  std::memset(C.data(), 0, C.size() * sizeof(float));

  for (auto _ : state) {
    aocl_gemm_bf16bf16f32of32(
        'R', // Row major
        'N', // No transpose A
        'N', // No transpose B
        M,
        N,
        K,
        1.0f, // alpha
        A.data(),
        K, // lda
        'N',
        B.data(),
        N, // ldb
        'N',
        0.0f, // beta
        C.data(),
        N, // ldc
        0);
    benchmark::DoNotOptimize(C.data());
  }

  const double flops = 2.0 * M * N * K;
  state.counters["FLOPS"] = benchmark::Counter(
      flops,
      benchmark::Counter::kIsIterationInvariantRate,
      benchmark::Counter::kIs1000);
}

void BM_HGEMM(benchmark::State& state) {
  state.SkipWithError("Not implemented because AOCL doesn't support FP16");
}

#endif

} // namespace

static void CustomArgs(benchmark::internal::Benchmark* b) {
  for (int size = kMinSize; size <= kMaxSize; size *= 2) {
    b->Args({size, size, size});
  }
}

#if defined(USE_APL_ACL)
BENCHMARK(BM_SGEMM_APL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_HGEMM_APL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_SGEMM_ACL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_HGEMM_ACL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_I8GEMM_ACL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_BF16GEMM_ACL)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
#else
BENCHMARK(BM_SGEMM)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_HGEMM)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_I8GEMM)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
BENCHMARK(BM_BF16GEMM)->Apply(CustomArgs)->ArgNames({"M", "N", "K"});
#endif

int main(int argc, char** argv) {
  //  Parse custom arguments before benchmark initialization
  int M = 0, N = 0, K = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
      M = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      N = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
      K = std::atoi(argv[++i]);
    }
  }

  if (M > 0 && N > 0 && K > 0) {
#if defined(USE_APL_ACL)
    benchmark::RegisterBenchmark("BM_SGEMM_APL", BM_SGEMM_APL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_HGEMM_APL", BM_HGEMM_APL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_SGEMM_ACL", BM_SGEMM_ACL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_HGEMM_ACL", BM_HGEMM_ACL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_I8GEMM_ACL", BM_I8GEMM_ACL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_BF16GEMM_ACL", BM_BF16GEMM_ACL)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
#else
    benchmark::RegisterBenchmark("BM_SGEMM", BM_SGEMM)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_HGEMM", BM_HGEMM)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_I8GEMM", BM_I8GEMM)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
    benchmark::RegisterBenchmark("BM_BF16GEMM", BM_BF16GEMM)
        ->Args({M, N, K})
        ->ArgNames({"M", "N", "K"});
#endif
  }

  // Remove our custom args so benchmark doesn't complain
  std::vector<char*> filtered_argv;
  filtered_argv.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if ((std::strcmp(argv[i], "-m") == 0 || std::strcmp(argv[i], "-n") == 0 ||
         std::strcmp(argv[i], "-k") == 0) &&
        i + 1 < argc) {
      ++i; // Skip the value
    } else {
      filtered_argv.push_back(argv[i]);
    }
  }

  int filtered_argc = static_cast<int>(filtered_argv.size());
  ::benchmark::Initialize(&filtered_argc, filtered_argv.data());

  if (::benchmark::ReportUnrecognizedArguments(
          filtered_argc, filtered_argv.data())) {
    return 1;
  }

  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();

  return 0;
}
