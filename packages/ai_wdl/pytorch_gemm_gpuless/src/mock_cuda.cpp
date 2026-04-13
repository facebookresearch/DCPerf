/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// mock_cuda.cpp — Patches libcuda.so.1 function table for GPU-less
// benchmarking.
//
// Based on Monarch's mock_cuda (D67496828) with updated pattern matching for
// NVIDIA driver 580.x+. See docs/driver_binary_analysis.md for details.
//
// Supports both x86_64 and aarch64 architectures:
//
// x86_64: Functions use cmpl with immediate 0x321cba00, then call/jmpq through
//   a RIP-relative function table pointer. We scan for the magic bytes
//   00 ba 1c 32, then for ff 15 (call) or ff 25 (jmpq).
//
// aarch64: Functions load 0x321cba00 via MOVZ+MOVK instructions, then use
//   ADRP+LDR to load the function pointer from a page-relative table, followed
//   by BLR for the indirect call. We scan for MOVZ Wn,#0xba00 + MOVK
//   Wn,#0x321c,LSL#16, then find B.EQ + ADRP + LDR + BLR.

// @lint-ignore-every CLANGSECURITY facebook-security-vulnerable-memcpy
// @lint-ignore-every CLANGTIDY clang-diagnostic-unused-parameter
#include <Python.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

// Maximum bytes to scan from function start when looking for patterns.
constexpr size_t SCAN_LIMIT = 128;
// Maximum number of 4-byte ARM instructions to scan.
constexpr size_t ARM_INSN_LIMIT = SCAN_LIMIT / 4;

#if defined(__x86_64__)

// --- x86_64 pattern matching ---

// The magic deinitialization check value as raw bytes (little-endian
// immediate).
const uint8_t MAGIC_BYTES[] = {0x00, 0xBA, 0x1C, 0x32};

// Opcode for indirect call through RIP-relative address: call *off(%rip)
const uint8_t CALL_INDIRECT[] = {0xFF, 0x15};

// Opcode for indirect jump through RIP-relative address: jmpq *off(%rip)
// Used by older driver versions.
const uint8_t JMPQ_INDIRECT[] = {0xFF, 0x25};

// Extract the function table entry address from an x86_64 CUDA driver function.
//
// Scans for the magic value 0x321cba00 as raw bytes, then for the first
// indirect call (ff 15) or jump (ff 25), and decodes the RIP-relative offset.
std::optional<void*> extractCallTarget(const uint8_t* functionBytes) {
  // Step 1: Find the magic value within the scan limit
  int magicOffset = -1;
  for (size_t i = 0; i + sizeof(MAGIC_BYTES) <= SCAN_LIMIT; ++i) {
    if (std::memcmp(functionBytes + i, MAGIC_BYTES, sizeof(MAGIC_BYTES)) == 0) {
      magicOffset = static_cast<int>(i);
      break;
    }
  }
  if (magicOffset < 0) {
    return std::nullopt;
  }

  // Step 2: Find the first indirect call (ff 15) or jump (ff 25) after the
  // magic
  size_t searchStart = magicOffset + sizeof(MAGIC_BYTES);
  for (size_t i = searchStart; i + 6 <= SCAN_LIMIT; ++i) {
    bool isCall =
        std::memcmp(functionBytes + i, CALL_INDIRECT, sizeof(CALL_INDIRECT)) ==
        0;
    bool isJmpq =
        std::memcmp(functionBytes + i, JMPQ_INDIRECT, sizeof(JMPQ_INDIRECT)) ==
        0;
    if (isCall || isJmpq) {
      // Step 3: Decode the 32-bit RIP-relative offset
      int32_t ripRelativeOffset;
      std::memcpy(&ripRelativeOffset, functionBytes + i + 2, sizeof(int32_t));

      // The RIP-relative offset is relative to the end of the instruction
      // (instruction address + 6 bytes for the full ff 15/25 XX XX XX XX)
      uintptr_t instructionAddress =
          reinterpret_cast<uintptr_t>(functionBytes) + i;
      uintptr_t targetAddress = instructionAddress + 6 + ripRelativeOffset;
      return reinterpret_cast<void*>(targetAddress);
    }
  }
  return std::nullopt;
}

#elif defined(__aarch64__)

// --- aarch64 pattern matching ---
//
// On ARM64, the magic 0x321cba00 is loaded via two instructions:
//   MOVZ Wn, #0xba00            → encodes as 0x52974000 | Rd
//   MOVK Wn, #0x321c, LSL #16  → encodes as 0x72a64380 | Rd
//
// After a CMP + B.EQ (deinitialization check), the function table entry
// is loaded via:
//   ADRP Xq, #page_offset      → page-relative address of table
//   LDR  Xq, [Xq, #offset]     → load function pointer from table
//   BLR  Xq                     → indirect call
//
// The function table address = (ADRP_PC & ~0xFFF) + (ADRP_imm << 12) +
// LDR_offset

// Instruction masks and expected values
// Instruction masks: mask out only Rd (low 5 bits) to match any register
constexpr uint32_t MOV_RD_MASK = 0xFFFFFFE0;
// MOVZ Wn, #0xba00, LSL #0: sf=0, opc=10, hw=00, imm16=0xba00
// 0_10_100101_00_1011101000000000_ddddd
constexpr uint32_t MOVZ_BA00_BASE = 0x52974000;

constexpr uint32_t MOVK_321C_BASE = 0x72A64380;

// B.EQ: 0101_0100_iiiiiiiiiiiiiiiiiii_0_0000
constexpr uint32_t BEQ_MASK = 0xFF00001F;
constexpr uint32_t BEQ_VAL = 0x54000000;

// ADRP: x_ii_10000_iiiiiiiiiiiiiiiiiii_ddddd
constexpr uint32_t ADRP_MASK = 0x9F000000;
constexpr uint32_t ADRP_VAL = 0x90000000;

// LDR X (64-bit, unsigned offset): 11_111_00101_iiiiiiiiiiii_nnnnn_ttttt
constexpr uint32_t LDR_X_MASK = 0xFFC00000;
constexpr uint32_t LDR_X_VAL = 0xF9400000;

// BLR: 1101_0110_0011_1111_0000_00nn_nnn0_0000
constexpr uint32_t BLR_MASK = 0xFFFFFC1F;
constexpr uint32_t BLR_VAL = 0xD63F0000;

// Decode ADRP immediate and compute target page address.
uintptr_t decodeAdrpTarget(uint32_t insn, uintptr_t pc) {
  // immhi = bits[23:5], immlo = bits[30:29]
  uint32_t immhi = (insn >> 5) & 0x7FFFF;
  uint32_t immlo = (insn >> 29) & 0x3;
  int64_t imm = static_cast<int64_t>((immhi << 2) | immlo);
  // Sign-extend from 21 bits
  if (imm & (1LL << 20)) {
    imm -= (1LL << 21);
  }
  return (pc & ~static_cast<uintptr_t>(0xFFF)) +
      (static_cast<uintptr_t>(imm) << 12);
}

// Extract the function table entry address from an aarch64 CUDA driver
// function.
std::optional<void*> extractCallTarget(const uint8_t* functionBytes) {
  const uint32_t* insns = reinterpret_cast<const uint32_t*>(functionBytes);

  // Step 1: Find MOVZ Wn, #0xba00 (the magic value load)
  int movzIdx = -1;
  uint32_t movzRd = 0;
  for (size_t i = 0; i < ARM_INSN_LIMIT; ++i) {
    if ((insns[i] & MOV_RD_MASK) == MOVZ_BA00_BASE) {
      movzRd = insns[i] & 0x1F;
      // Verify MOVK with matching Rd exists nearby (within 8 instructions)
      for (size_t j = i + 1; j < i + 8 && j < ARM_INSN_LIMIT; ++j) {
        if ((insns[j] & MOV_RD_MASK) == MOVK_321C_BASE &&
            (insns[j] & 0x1F) == movzRd) {
          movzIdx = static_cast<int>(i);
          break;
        }
      }
      if (movzIdx >= 0) {
        break;
      }
    }
  }
  if (movzIdx < 0) {
    return std::nullopt;
  }

  // Step 2: Find B.EQ after the MOVZ/MOVK pair
  int beqIdx = -1;
  for (size_t i = movzIdx + 2; i < ARM_INSN_LIMIT; ++i) {
    if ((insns[i] & BEQ_MASK) == BEQ_VAL) {
      beqIdx = static_cast<int>(i);
      break;
    }
  }
  if (beqIdx < 0) {
    return std::nullopt;
  }

  // Step 3: After B.EQ, find ADRP Xq (loads page of function table)
  for (size_t i = beqIdx + 1; i < ARM_INSN_LIMIT; ++i) {
    if ((insns[i] & ADRP_MASK) != ADRP_VAL) {
      continue;
    }
    uint32_t adrpRd = insns[i] & 0x1F;
    uintptr_t adrpPc = reinterpret_cast<uintptr_t>(functionBytes) + i * 4;
    uintptr_t pageAddr = decodeAdrpTarget(insns[i], adrpPc);

    // Step 4: Find LDR Xt, [Xq, #offset] where Xq matches ADRP Rd
    for (size_t j = i + 1; j < i + 8 && j < ARM_INSN_LIMIT; ++j) {
      if ((insns[j] & LDR_X_MASK) != LDR_X_VAL) {
        continue;
      }
      uint32_t ldrRn = (insns[j] >> 5) & 0x1F;
      if (ldrRn != adrpRd) {
        continue;
      }
      uint32_t ldrImm12 = (insns[j] >> 10) & 0xFFF;
      uint32_t ldrOffset = ldrImm12 * 8; // 64-bit LDR scales by 8
      uint32_t ldrRt = insns[j] & 0x1F;

      // Step 5: Verify BLR Xt follows (within a few instructions)
      for (size_t k = j + 1; k < j + 8 && k < ARM_INSN_LIMIT; ++k) {
        if ((insns[k] & BLR_MASK) == BLR_VAL) {
          uint32_t blrRn = (insns[k] >> 5) & 0x1F;
          if (blrRn == ldrRt) {
            uintptr_t tableAddr = pageAddr + ldrOffset;
            return reinterpret_cast<void*>(tableAddr);
          }
        }
      }
      // Even without BLR confirmation, if ADRP+LDR pattern matches
      // and it's the first one after B.EQ, trust it
      uintptr_t tableAddr = pageAddr + ldrOffset;
      return reinterpret_cast<void*>(tableAddr);
    }
  }
  return std::nullopt;
}

#else
#error "mock_cuda: unsupported architecture (need x86_64 or aarch64)"
#endif

// Swap the function pointer at the table entry with our replacement.
// Returns the original function pointer that was in the table.
std::optional<void*> swapCallTarget(void* functionAddr, void* newTarget) {
  uint8_t* functionBytes = reinterpret_cast<uint8_t*>(functionAddr);
  auto targetAddressOpt = extractCallTarget(functionBytes);
  if (!targetAddressOpt) {
    return std::nullopt;
  }
  std::atomic<void*>* atomicTargetAddress =
      reinterpret_cast<std::atomic<void*>*>(*targetAddressOpt);
  return atomicTargetAddress->exchange(newTarget);
}

// --- Function list and mock implementations ---
// Mirrors Monarch's FORALL_FUNCTIONS macro from D67496828.

// Core functions — must be direct exports in libcuda.so.1.
#define FORALL_CORE_FUNCTIONS(_) \
  _(cuGetProcAddress)            \
  _(cuInit)                      \
  _(cuDriverGetVersion)          \
  _(cuDeviceGetCount)            \
  _(cuDeviceGet)                 \
  _(cuDeviceGetAttribute)        \
  _(cuDeviceGetName)             \
  _(cuDeviceTotalMem)            \
  _(cuLaunchKernel)              \
  _(cuMemcpyDtoHAsync)           \
  _(cuMemcpyHtoDAsync)           \
  _(cuMemsetD8Async)             \
  _(cuLaunchKernelEx)            \
  _(cuMemAlloc)                  \
  _(cuMemFree)                   \
  _(cuMemcpyDtoDAsync)           \
  _(cuPointerGetAttribute)       \
  _(cuGetProcAddress_v2)         \
  _(cuMemCreate)                 \
  _(cuMemAddressReserve)         \
  _(cuMemMap)                    \
  _(cuMemSetAccess)              \
  _(cuMemcpyAsync)               \
  _(cuMemRelease)                \
  _(cuMemUnmap)                  \
  _(cuMemAddressFree)            \
  _(cuMemRetainAllocationHandle) \
  _(cuMemGetAddressRange)        \
  _(cuDeviceComputeCapability)   \
  _(cuDeviceGetProperties)       \
  _(cuGetExportTable)

// Optional functions — may only be available through cuGetProcAddress.
// Patched lazily when resolved via cuGetProcAddress, or eagerly if
// found as direct exports.
#define FORALL_OPTIONAL_FUNCTIONS(_) \
  _(cuModuleLoadData)                \
  _(cuModuleLoadDataEx)              \
  _(cuModuleLoadFatBinary)           \
  _(cuModuleGetFunction)             \
  _(cuModuleUnload)                  \
  _(cuCtxCreate_v2)                  \
  _(cuCtxGetCurrent)                 \
  _(cuCtxSetCurrent)                 \
  _(cuCtxSynchronize)                \
  _(cuCtxDestroy_v2)                 \
  _(cuStreamCreate)                  \
  _(cuStreamCreateWithFlags)         \
  _(cuStreamCreateWithPriority)      \
  _(cuStreamSynchronize)             \
  _(cuStreamDestroy_v2)              \
  _(cuStreamWaitEvent)               \
  _(cuStreamGetCaptureInfo)          \
  _(cuEventCreate)                   \
  _(cuEventRecord)                   \
  _(cuEventSynchronize)              \
  _(cuEventDestroy_v2)               \
  _(cuEventQuery)

// All functions — used for mock definitions and cuGetProcAddress interception.
#define FORALL_FUNCTIONS(_) \
  FORALL_CORE_FUNCTIONS(_)  \
  FORALL_OPTIONAL_FUNCTIONS(_)

// Each function can have up to MAX_VERSIONS entries in the driver's function
// table (different entries for different CUDA API versions returned by
// cuGetProcAddress). We need a separate mock for each version so that
// RETURN_REAL_IF_UNMOCKED can call back to the correct real function.
constexpr int MAX_VERSIONS = 8;

#define CREATE_REALS(fn)                     \
  void* real_##fn[MAX_VERSIONS] = {nullptr}; \
  extern void* ps_##fn[];

FORALL_FUNCTIONS(CREATE_REALS)
#undef CREATE_REALS

// --- CUDA type definitions (minimal, matching driver API) ---

using CUresult = int;
using CUdevice = int;
using CUmemGenericAllocationHandle = unsigned long long;
using CUcontext = struct CUctx_st*;
using CUfunction = struct CUfunc_st*;
using CUmodule = struct CUmod_st*;
using CUstream = struct CUstream_st*;
using CUevent = struct CUevent_st*;
struct CUlaunchConfig;
struct CUmemAllocationProp;
struct CUmemAccessDesc;

// CUuuid — 16-byte GUID used by cuGetExportTable.
struct CUuuid {
  char bytes[16];
};

enum CUpointer_attribute {
  CU_POINTER_ATTRIBUTE_CONTEXT = 1,
  CU_POINTER_ATTRIBUTE_MEMORY_TYPE = 2,
  CU_POINTER_ATTRIBUTE_DEVICE_POINTER = 3,
  CU_POINTER_ATTRIBUTE_HOST_POINTER = 4,
  CU_POINTER_ATTRIBUTE_P2P_TOKENS = 5,
  CU_POINTER_ATTRIBUTE_SYNC_MEMOPS = 6,
  CU_POINTER_ATTRIBUTE_BUFFER_ID = 7,
  CU_POINTER_ATTRIBUTE_IS_MANAGED = 8,
  CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL = 9,
  CU_POINTER_ATTRIBUTE_IS_LEGACY_CUDA_IPC_CAPABLE = 10,
  CU_POINTER_ATTRIBUTE_RANGE_START_ADDR = 11,
  CU_POINTER_ATTRIBUTE_RANGE_SIZE = 12,
  CU_POINTER_ATTRIBUTE_MAPPED = 13,
  CU_POINTER_ATTRIBUTE_ALLOWED_HANDLE_TYPES = 14,
  CU_POINTER_ATTRIBUTE_IS_GPU_DIRECT_RDMA_CAPABLE = 15,
  CU_POINTER_ATTRIBUTE_ACCESS_FLAGS = 16,
  CU_POINTER_ATTRIBUTE_MEMPOOL_HANDLE = 17,
  CU_POINTER_ATTRIBUTE_MAPPING_SIZE = 18,
  CU_POINTER_ATTRIBUTE_MAPPING_BASE_ADDR = 19,
  CU_POINTER_ATTRIBUTE_MEMORY_BLOCK_ID = 20
};

// Thread-local mock toggle. When true, intercepted CUDA calls return
// success immediately without doing real GPU work. When false, they
// forward to the real driver function.
thread_local std::atomic<bool> mockCudaEnabled = false;

#define RETURN_REAL_IF_UNMOCKED(fn, ...)                      \
  if (!mockCudaEnabled.load()) {                              \
    return ((decltype(&p_##fn<N>))real_##fn[N])(__VA_ARGS__); \
  }

// --- Mock function implementations ---
// Each is a template parameterized by version index N, so different
// driver table entries can call back to the correct real function.

// Tracks whether the real NVIDIA driver initialized successfully.
// When true, mocked functions call through to the real driver for
// consistency (GPU machine). When false, everything is fully faked
// (GPU-less machine with only cuda-compat stub).
std::atomic<bool> realDriverOK{false};

// GPU-less init mocks: return success with fake device info
template <int N>
CUresult p_cuInit(unsigned int flags) {
  fprintf(
      stderr,
      "[mock_cuda] cuInit ENTERED, mock=%d\n",
      mockCudaEnabled.load() ? 1 : 0);
  RETURN_REAL_IF_UNMOCKED(cuInit, flags);
  return 0; // CUDA_SUCCESS
}

template <int N>
CUresult p_cuDriverGetVersion(int* version) {
  RETURN_REAL_IF_UNMOCKED(cuDriverGetVersion, version);
  fprintf(stderr, "[mock_cuda] cuDriverGetVersion called\n");
  if (realDriverOK.load()) {
    auto r = ((decltype(&p_cuDriverGetVersion<N>))real_cuDriverGetVersion[N])(
        version);
    if (r == 0)
      return 0;
  }
  *version = 99999;
  return 0;
}

template <int N>
CUresult p_cuDeviceGetCount(int* count) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceGetCount, count);
  fprintf(stderr, "[mock_cuda] cuDeviceGetCount called\n");
  if (realDriverOK.load()) {
    auto r =
        ((decltype(&p_cuDeviceGetCount<N>))real_cuDeviceGetCount[N])(count);
    if (r == 0 && *count > 0)
      return 0;
  }
  *count = 1;
  return 0;
}

template <int N>
CUresult p_cuDeviceGet(CUdevice* device, int ordinal) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceGet, device, ordinal);
  if (realDriverOK.load()) {
    auto r =
        ((decltype(&p_cuDeviceGet<N>))real_cuDeviceGet[N])(device, ordinal);
    if (r == 0)
      return 0;
  }
  *device = ordinal;
  return 0;
}

// Target compute capability for the mock. Set to sm_90 which is widely
// supported by CUDA 12.x builds. This override is applied even when mock
// is NOT fully enabled, so that cudart caches sm_90 during CUDA init.
// This prevents cudaErrorNoKernelImageForDevice on GPUs whose arch is
// too new for the cuBLAS build (e.g. GB200 sm_100 with CUDA 12.4).
constexpr int MOCK_CC_MAJOR = 9;
constexpr int MOCK_CC_MINOR = 0;

template <int N>
CUresult p_cuDeviceGetAttribute(int* value, int attribute, CUdevice dev) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceGetAttribute, value, attribute, dev);
  if (realDriverOK.load()) {
    auto r =
        ((decltype(&p_cuDeviceGetAttribute<N>))real_cuDeviceGetAttribute[N])(
            value, attribute, dev);
    if (r == 0)
      return 0;
  }
  // GPU-less fallback — return reasonable defaults
  switch (attribute) {
    case 1:
      *value = 1024;
      break; // MAX_THREADS_PER_BLOCK
    case 14:
      *value = 32;
      break; // WARP_SIZE
    case 21:
      *value = MOCK_CC_MAJOR;
      break; // COMPUTE_CAPABILITY_MAJOR
    case 22:
      *value = MOCK_CC_MINOR;
      break; // COMPUTE_CAPABILITY_MINOR
    case 30:
      *value = 132;
      break; // MULTIPROCESSOR_COUNT
    case 75:
      *value = MOCK_CC_MAJOR;
      break; // COMPUTE_CAPABILITY_MAJOR (alt)
    case 76:
      *value = MOCK_CC_MINOR;
      break; // COMPUTE_CAPABILITY_MINOR (alt)
    default:
      *value = 0;
      break;
  }
  return 0;
}

// Deprecated function — cudart may use this instead of cuDeviceGetAttribute.
template <int N>
CUresult p_cuDeviceComputeCapability(int* major, int* minor, CUdevice dev) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceComputeCapability, major, minor, dev);
  if (realDriverOK.load()) {
    auto r = ((decltype(&p_cuDeviceComputeCapability<N>))
                  real_cuDeviceComputeCapability[N])(major, minor, dev);
    if (r == 0)
      return 0;
  }
  *major = MOCK_CC_MAJOR;
  *minor = MOCK_CC_MINOR;
  return 0;
}

// CUdevprop is an old-style device properties struct.
// We mock the compute capability fields but zero the rest.
struct CUdevprop {
  int maxThreadsPerBlock;
  int maxThreadsDim[3];
  int maxGridSize[3];
  int sharedMemPerBlock;
  int totalConstantMemory;
  int SIMDWidth;
  int memPitch;
  int regsPerBlock;
  int clockRate;
  int textureAlign;
};

template <int N>
CUresult p_cuDeviceGetProperties(CUdevprop* prop, CUdevice dev) {
  if (!mockCudaEnabled.load()) {
    return (
        (decltype(&p_cuDeviceGetProperties<N>))real_cuDeviceGetProperties[N])(
        prop, dev);
  }
  memset(prop, 0, sizeof(CUdevprop));
  prop->maxThreadsPerBlock = 1024;
  prop->SIMDWidth = 32;
  return 0;
}

template <int N>
CUresult p_cuDeviceGetName(char* name, int len, CUdevice dev) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceGetName, name, len, dev);
  if (realDriverOK.load()) {
    auto r = ((decltype(&p_cuDeviceGetName<N>))real_cuDeviceGetName[N])(
        name, len, dev);
    if (r == 0)
      return 0;
  }
  snprintf(name, len, "Mock CUDA Device (GPU-less benchmark)");
  return 0;
}

template <int N>
CUresult p_cuDeviceTotalMem(size_t* bytes, CUdevice dev) {
  RETURN_REAL_IF_UNMOCKED(cuDeviceTotalMem, bytes, dev);
  if (realDriverOK.load()) {
    auto r = ((decltype(&p_cuDeviceTotalMem<N>))real_cuDeviceTotalMem[N])(
        bytes, dev);
    if (r == 0)
      return 0;
  }
  *bytes = 80ULL * 1024 * 1024 * 1024; // 80 GB
  return 0;
}

template <int N>
CUresult p_cuLaunchKernel(
    CUfunction f,
    unsigned int gridDimX,
    unsigned int gridDimY,
    unsigned int gridDimZ,
    unsigned int blockDimX,
    unsigned int blockDimY,
    unsigned int blockDimZ,
    unsigned int sharedMemBytes,
    CUstream hStream,
    void** kernelParams,
    void** extra) {
  RETURN_REAL_IF_UNMOCKED(
      cuLaunchKernel,
      f,
      gridDimX,
      gridDimY,
      gridDimZ,
      blockDimX,
      blockDimY,
      blockDimZ,
      sharedMemBytes,
      hStream,
      kernelParams,
      extra);
  return 0;
}

std::mutex lockMemAddr;
size_t memAddr = static_cast<size_t>(1UL << 48);

template <int N>
CUresult p_cuMemAlloc(CUdevice** dptr, size_t bytesize) {
  RETURN_REAL_IF_UNMOCKED(cuMemAlloc, dptr, bytesize);
  std::lock_guard<std::mutex> guard(lockMemAddr);
  memAddr -= bytesize;
  memAddr -= memAddr % 8;
  *dptr = reinterpret_cast<CUdevice*>(memAddr);
  return 0;
}

template <int N>
CUresult p_cuMemFree(CUdevice* dptr) {
  RETURN_REAL_IF_UNMOCKED(cuMemFree, dptr);
  return 0;
}

template <int N>
CUresult p_cuMemcpyDtoDAsync(
    CUdevice* dstDevice,
    CUdevice* srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(
      cuMemcpyDtoDAsync, dstDevice, srcDevice, ByteCount, hStream);
  return 0;
}

template <int N>
CUresult p_cuLaunchKernelEx(
    const CUlaunchConfig* config,
    CUfunction f,
    void** kernelParams,
    void** extra) {
  RETURN_REAL_IF_UNMOCKED(cuLaunchKernelEx, config, f, kernelParams, extra);
  return 0;
}

template <int N>
CUresult p_cuMemcpyDtoHAsync(
    void* dstHost,
    CUdevice* srcDevice,
    size_t ByteCount,
    CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(
      cuMemcpyDtoHAsync, dstHost, srcDevice, ByteCount, hStream);
  return 0;
}

template <int N>
CUresult p_cuMemcpyHtoDAsync(
    CUdevice* dstDevice,
    const void* srcHost,
    size_t ByteCount,
    CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(
      cuMemcpyHtoDAsync, dstDevice, srcHost, ByteCount, hStream);
  return 0;
}

template <int N>
CUresult p_cuMemsetD8Async(
    CUdevice* dstDevice,
    unsigned char uc,
    size_t M,
    CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(cuMemsetD8Async, dstDevice, uc, M, hStream);
  return 0;
}

template <int N>
CUresult p_cuPointerGetAttribute(
    void* data,
    CUpointer_attribute attribute,
    CUdevice* ptr) {
  RETURN_REAL_IF_UNMOCKED(cuPointerGetAttribute, data, attribute, ptr);
  return 0;
}

namespace {
std::random_device _rd;
std::mt19937_64 _gen(_rd());
std::uniform_int_distribution<uint64_t> _dis;

uint64_t randUint64_t() {
  return _dis(_gen);
}
} // namespace

template <int N>
CUresult p_cuMemCreate(
    CUmemGenericAllocationHandle* handle,
    size_t size,
    const CUmemAllocationProp* prop,
    unsigned long long flags) {
  RETURN_REAL_IF_UNMOCKED(cuMemCreate, handle, size, prop, flags);
  *handle = randUint64_t();
  return 0;
}

template <int N>
CUresult p_cuMemRelease(CUmemGenericAllocationHandle handle) {
  RETURN_REAL_IF_UNMOCKED(cuMemRelease, handle);
  return 0;
}

static std::unordered_map<CUdevice*, size_t> ptrToSize;

template <int N>
CUresult p_cuMemAddressReserve(
    CUdevice** ptr,
    size_t size,
    size_t alignment,
    CUdevice* addr,
    unsigned long long flags) {
  RETURN_REAL_IF_UNMOCKED(
      cuMemAddressReserve, ptr, size, alignment, addr, flags);
  std::lock_guard<std::mutex> guard(lockMemAddr);
  memAddr -= size;
  size_t offset = memAddr % (alignment ? alignment : 8);
  memAddr -= offset;
  *ptr = reinterpret_cast<CUdevice*>(memAddr);
  ptrToSize[*ptr] = size + offset;
  return 0;
}

template <int N>
CUresult p_cuMemMap(
    CUdevice* ptr,
    size_t size,
    size_t offset,
    CUmemGenericAllocationHandle handle,
    unsigned long long flags) {
  RETURN_REAL_IF_UNMOCKED(cuMemMap, ptr, size, offset, handle, flags);
  return 0;
}

template <int N>
CUresult p_cuMemUnmap(CUdevice* ptr, size_t size) {
  RETURN_REAL_IF_UNMOCKED(cuMemUnmap, ptr, size);
  return 0;
}

template <int N>
CUresult p_cuMemSetAccess(
    CUdevice* ptr,
    size_t size,
    const CUmemAccessDesc* desc,
    size_t count) {
  RETURN_REAL_IF_UNMOCKED(cuMemSetAccess, ptr, size, desc, count);
  return 0;
}

template <int N>
CUresult p_cuMemcpyAsync(
    CUdevice* dst,
    CUdevice* src,
    size_t ByteCount,
    CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(cuMemcpyAsync, dst, src, ByteCount, hStream);
  return 0;
}

template <int N>
CUresult p_cuMemAddressFree(CUdevice* ptr, size_t size) {
  RETURN_REAL_IF_UNMOCKED(cuMemAddressFree, ptr, size);
  return 0;
}

template <int N>
CUresult p_cuMemRetainAllocationHandle(
    CUmemGenericAllocationHandle* handle,
    void* addr) {
  RETURN_REAL_IF_UNMOCKED(cuMemRetainAllocationHandle, handle, addr);
  return 0;
}

template <int N>
CUresult
p_cuMemGetAddressRange(CUdevice** pbase, size_t* psize, CUdevice* dptr) {
  RETURN_REAL_IF_UNMOCKED(cuMemGetAddressRange, pbase, psize, dptr);
  auto it = ptrToSize.find(dptr);
  if (it != ptrToSize.end()) {
    if (pbase) {
      *pbase = dptr;
    }
    if (psize) {
      *psize = it->second;
    }
    return 0;
  } else {
    for (const auto& entry : ptrToSize) {
      CUdevice* base = entry.first;
      size_t size = entry.second;
      if (dptr >= base &&
          dptr < reinterpret_cast<CUdevice*>(
                     reinterpret_cast<char*>(base) + size)) {
        if (pbase) {
          *pbase = base;
        }
        if (psize) {
          *psize = size;
        }
        return 0;
      }
    }
    return 1; // CUDA_ERROR_INVALID_VALUE
  }
}

// --- Module loading mocks (for cuBLAS/cuDNN init on unsupported GPUs) ---

// Fake module/function handles — non-null sentinels cast from integers.
static CUmodule fakeModule = reinterpret_cast<CUmodule>(0xDEAD0001);
static CUfunction fakeFunc = reinterpret_cast<CUfunction>(0xDEAD0002);

template <int N>
CUresult p_cuModuleLoadData(CUmodule* module, const void* image) {
  RETURN_REAL_IF_UNMOCKED(cuModuleLoadData, module, image);
  *module = fakeModule;
  return 0;
}

template <int N>
CUresult p_cuModuleLoadDataEx(
    CUmodule* module,
    const void* image,
    unsigned int numOptions,
    void* options,
    void** optionValues) {
  RETURN_REAL_IF_UNMOCKED(
      cuModuleLoadDataEx, module, image, numOptions, options, optionValues);
  *module = fakeModule;
  return 0;
}

template <int N>
CUresult p_cuModuleLoadFatBinary(CUmodule* module, const void* fatCubin) {
  RETURN_REAL_IF_UNMOCKED(cuModuleLoadFatBinary, module, fatCubin);
  *module = fakeModule;
  return 0;
}

template <int N>
CUresult
p_cuModuleGetFunction(CUfunction* hfunc, CUmodule hmod, const char* name) {
  RETURN_REAL_IF_UNMOCKED(cuModuleGetFunction, hfunc, hmod, name);
  *hfunc = fakeFunc;
  return 0;
}

template <int N>
CUresult p_cuModuleUnload(CUmodule hmod) {
  RETURN_REAL_IF_UNMOCKED(cuModuleUnload, hmod);
  return 0;
}

// --- Context and stream mocks ---
// All try real driver first, fall back to fake values on failure.
// This lets them work on both GPU and GPU-less machines.

// Helper: try real driver function if driver is OK, return 0 if it succeeds.
#define TRY_REAL(fn, ...)                                        \
  if (realDriverOK.load() && real_##fn[N]) {                     \
    auto _r = ((decltype(&p_##fn<N>))real_##fn[N])(__VA_ARGS__); \
    if (_r == 0)                                                 \
      return 0;                                                  \
  }

static CUcontext fakeCtx = reinterpret_cast<CUcontext>(0xDEAD0010);
static std::atomic<uintptr_t> nextStream{0xDEAD1000};
static std::atomic<uintptr_t> nextEvent{0xDEAD2000};

template <int N>
CUresult p_cuCtxCreate_v2(CUcontext* pctx, unsigned int flags, CUdevice dev) {
  RETURN_REAL_IF_UNMOCKED(cuCtxCreate_v2, pctx, flags, dev);
  TRY_REAL(cuCtxCreate_v2, pctx, flags, dev);
  *pctx = fakeCtx;
  return 0;
}

template <int N>
CUresult p_cuCtxGetCurrent(CUcontext* pctx) {
  RETURN_REAL_IF_UNMOCKED(cuCtxGetCurrent, pctx);
  TRY_REAL(cuCtxGetCurrent, pctx);
  *pctx = fakeCtx;
  return 0;
}

template <int N>
CUresult p_cuCtxSetCurrent(CUcontext ctx) {
  RETURN_REAL_IF_UNMOCKED(cuCtxSetCurrent, ctx);
  TRY_REAL(cuCtxSetCurrent, ctx);
  return 0;
}

template <int N>
CUresult p_cuCtxSynchronize() {
  RETURN_REAL_IF_UNMOCKED(cuCtxSynchronize);
  TRY_REAL(cuCtxSynchronize);
  return 0;
}

template <int N>
CUresult p_cuCtxDestroy_v2(CUcontext ctx) {
  RETURN_REAL_IF_UNMOCKED(cuCtxDestroy_v2, ctx);
  return 0;
}

template <int N>
CUresult p_cuStreamCreate(CUstream* phStream, unsigned int flags) {
  RETURN_REAL_IF_UNMOCKED(cuStreamCreate, phStream, flags);
  TRY_REAL(cuStreamCreate, phStream, flags);
  *phStream = reinterpret_cast<CUstream>(nextStream.fetch_add(1));
  return 0;
}

template <int N>
CUresult p_cuStreamCreateWithFlags(CUstream* phStream, unsigned int flags) {
  RETURN_REAL_IF_UNMOCKED(cuStreamCreateWithFlags, phStream, flags);
  TRY_REAL(cuStreamCreateWithFlags, phStream, flags);
  *phStream = reinterpret_cast<CUstream>(nextStream.fetch_add(1));
  return 0;
}

template <int N>
CUresult p_cuStreamCreateWithPriority(
    CUstream* phStream,
    unsigned int flags,
    int priority) {
  RETURN_REAL_IF_UNMOCKED(
      cuStreamCreateWithPriority, phStream, flags, priority);
  TRY_REAL(cuStreamCreateWithPriority, phStream, flags, priority);
  *phStream = reinterpret_cast<CUstream>(nextStream.fetch_add(1));
  return 0;
}

template <int N>
CUresult p_cuStreamSynchronize(CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(cuStreamSynchronize, hStream);
  return 0;
}

template <int N>
CUresult p_cuStreamDestroy_v2(CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(cuStreamDestroy_v2, hStream);
  return 0;
}

template <int N>
CUresult p_cuStreamWaitEvent(CUstream hStream, CUevent hEvent, unsigned int f) {
  RETURN_REAL_IF_UNMOCKED(cuStreamWaitEvent, hStream, hEvent, f);
  return 0;
}

template <int N>
CUresult
p_cuStreamGetCaptureInfo(CUstream hStream, int* captureStatus, uint64_t* id) {
  RETURN_REAL_IF_UNMOCKED(cuStreamGetCaptureInfo, hStream, captureStatus, id);
  *captureStatus = 0; // CU_STREAM_CAPTURE_STATUS_NONE
  if (id) {
    *id = 0;
  }
  return 0;
}

template <int N>
CUresult p_cuEventCreate(CUevent* phEvent, unsigned int flags) {
  RETURN_REAL_IF_UNMOCKED(cuEventCreate, phEvent, flags);
  *phEvent = reinterpret_cast<CUevent>(nextEvent.fetch_add(1));
  return 0;
}

template <int N>
CUresult p_cuEventRecord(CUevent hEvent, CUstream hStream) {
  RETURN_REAL_IF_UNMOCKED(cuEventRecord, hEvent, hStream);
  return 0;
}

template <int N>
CUresult p_cuEventSynchronize(CUevent hEvent) {
  RETURN_REAL_IF_UNMOCKED(cuEventSynchronize, hEvent);
  return 0;
}

template <int N>
CUresult p_cuEventDestroy_v2(CUevent hEvent) {
  RETURN_REAL_IF_UNMOCKED(cuEventDestroy_v2, hEvent);
  return 0;
}

template <int N>
CUresult p_cuEventQuery(CUevent hEvent) {
  RETURN_REAL_IF_UNMOCKED(cuEventQuery, hEvent);
  return 0;
}

// --- cuGetExportTable mock ---
//
// cuGetExportTable is called by cudart during initialization with
// undocumented 16-byte GUIDs. It returns internal vtable-like structures
// whose function pointers cudart uses for its own init.
//
// On GPU-less machines (or when mock is enabled before CUDA init), the
// real cuGetExportTable fails. We provide a fallback: a dummy table
// filled with no-op function stubs that return CUDA_SUCCESS. This lets
// cudart's init sequence proceed without crashing.
//
// The table is sized generously (1024 entries) since we don't know how
// many function pointers each GUID's table contains. Functions called
// through the table will return 0 (success) and leave output parameters
// unmodified.

// Indexed no-op stubs for export table entries. Each stub logs its
// table+index when called, helping us identify which entries cudart uses.
// On aarch64, the first 8 args are in x0-x7 regardless of type.
constexpr size_t EXPORT_TABLE_SIZE = 1024;

static CUresult noop_export_fn(void) {
  return 0; // CUDA_SUCCESS
}

// Per-entry logging stubs — each template instantiation is a unique function
// pointer, so we can track which table entry gets called. The table ID (T)
// identifies which GUID's table, and the entry index (I) identifies which
// slot. We log via fprintf and return 0.
template <int T, int I>
static CUresult logged_export_fn(
    void* a0 = nullptr,
    void* a1 = nullptr,
    void* a2 = nullptr,
    void* a3 = nullptr) {
  fprintf(
      stderr,
      "[mock_cuda] EXPORT CALL: table=%d entry=%d args=(%p, %p, %p, %p)\n",
      T,
      I,
      a0,
      a1,
      a2,
      a3);
  return 0;
}

// Helper to fill a table with logged stubs for a specific table ID.
template <int T>
static void fillLoggedTable(void** table) {
  // entry[0] is table size
  table[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(256));
  // Fill entries 1-31 with unique logged stubs.
  table[1] = reinterpret_cast<void*>(&logged_export_fn<T, 1>);
  table[2] = reinterpret_cast<void*>(&logged_export_fn<T, 2>);
  table[3] = reinterpret_cast<void*>(&logged_export_fn<T, 3>);
  table[4] = reinterpret_cast<void*>(&logged_export_fn<T, 4>);
  table[5] = reinterpret_cast<void*>(&logged_export_fn<T, 5>);
  table[6] = reinterpret_cast<void*>(&logged_export_fn<T, 6>);
  table[7] = reinterpret_cast<void*>(&logged_export_fn<T, 7>);
  table[8] = reinterpret_cast<void*>(&logged_export_fn<T, 8>);
  table[9] = reinterpret_cast<void*>(&logged_export_fn<T, 9>);
  table[10] = reinterpret_cast<void*>(&logged_export_fn<T, 10>);
  table[11] = reinterpret_cast<void*>(&logged_export_fn<T, 11>);
  table[12] = reinterpret_cast<void*>(&logged_export_fn<T, 12>);
  table[13] = reinterpret_cast<void*>(&logged_export_fn<T, 13>);
  table[14] = reinterpret_cast<void*>(&logged_export_fn<T, 14>);
  table[15] = reinterpret_cast<void*>(&logged_export_fn<T, 15>);
  for (size_t i = 16; i < EXPORT_TABLE_SIZE; i++) {
    table[i] = reinterpret_cast<void*>(&noop_export_fn);
  }
}

// We need multiple no-op stubs with different signatures for functions
// that cudart calls with output parameters. These return 0 and write
// reasonable defaults.
static CUresult noop_export_fn_1ptr(void** out) {
  if (out) {
    *out = nullptr;
  }
  return 0;
}

static CUresult noop_export_fn_1int(int* out) {
  if (out) {
    *out = 0;
  }
  return 0;
}

static CUresult noop_export_fn_1size(size_t* out) {
  if (out) {
    *out = 0;
  }
  return 0;
}

// Per-GUID export table cache.
static std::mutex exportTableMutex;
static std::unordered_map<std::string, void*> exportTableCache;

static std::atomic<int> nextTableId{0};

static void* getOrCreateExportTable(const CUuuid* pExportTableId) {
  std::string key(pExportTableId->bytes, 16);
  std::lock_guard<std::mutex> guard(exportTableMutex);
  auto it = exportTableCache.find(key);
  if (it != exportTableCache.end()) {
    return it->second;
  }
  auto** table = static_cast<void**>(calloc(EXPORT_TABLE_SIZE, sizeof(void*)));
  int tid = nextTableId.fetch_add(1);
  // Fill with logged stubs based on table ID.
  switch (tid) {
    case 0:
      fillLoggedTable<0>(table);
      break;
    case 1:
      fillLoggedTable<1>(table);
      break;
    case 2:
      fillLoggedTable<2>(table);
      break;
    case 3:
      fillLoggedTable<3>(table);
      break;
    default:
      // Fallback for additional tables.
      table[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(256));
      for (size_t i = 1; i < EXPORT_TABLE_SIZE; i++) {
        table[i] = reinterpret_cast<void*>(&noop_export_fn);
      }
      break;
  }
  exportTableCache[key] = static_cast<void*>(table);
  return static_cast<void*>(table);
}

template <int N>
CUresult p_cuGetExportTable(
    const void** ppExportTable,
    const CUuuid* pExportTableId) {
  if (!mockCudaEnabled.load()) {
    return ((decltype(&p_cuGetExportTable<N>))real_cuGetExportTable[N])(
        ppExportTable, pExportTableId);
  }
  // Log the GUID for debugging.
  const auto* b = reinterpret_cast<const unsigned char*>(pExportTableId->bytes);
  fprintf(
      stderr,
      "[mock_cuda] cuGetExportTable GUID: "
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
      " realDriverOK=%d\n",
      b[0],
      b[1],
      b[2],
      b[3],
      b[4],
      b[5],
      b[6],
      b[7],
      b[8],
      b[9],
      b[10],
      b[11],
      b[12],
      b[13],
      b[14],
      b[15],
      realDriverOK.load() ? 1 : 0);
  // Try real driver first — if it succeeds, use real tables.
  if (realDriverOK.load() && real_cuGetExportTable[N]) {
    auto r = ((decltype(&p_cuGetExportTable<N>))real_cuGetExportTable[N])(
        ppExportTable, pExportTableId);
    fprintf(stderr, "[mock_cuda]   real cuGetExportTable returned %d\n", r);
    if (r == 0) {
      // Dump table entries for reverse-engineering.
      auto** tbl = const_cast<void**>(
          reinterpret_cast<const void* const*>(*ppExportTable));
      fprintf(stderr, "[mock_cuda]   table at %p, entries:\n", *ppExportTable);
      for (int i = 0; i < 32 && tbl[i]; i++) {
        fprintf(stderr, "[mock_cuda]     [%2d] = %p\n", i, tbl[i]);
      }
      return 0;
    }
  }
  // Real driver failed (GPU-less machine) — provide dummy table.
  *ppExportTable = getOrCreateExportTable(pExportTableId);
  fprintf(stderr, "[mock_cuda]   using DUMMY table at %p\n", *ppExportTable);
  return 0;
}

// --- Patching infrastructure ---

std::unordered_set<void*> patched;
std::mutex patchedMutex;

void doPatch(const char* name, void** realFns, void* toPatch, void** ourFns) {
  std::lock_guard<std::mutex> guard(patchedMutex);
  if (patched.count(toPatch)) {
    return;
  }
  patched.emplace(toPatch);
  for (size_t i = 0; i < MAX_VERSIONS; ++i) {
    if (realFns[i] == nullptr) {
      auto result = swapCallTarget(toPatch, ourFns[i]);
      if (!result) {
        throw std::runtime_error(
            std::string("Failed to patch CUDA function: ") + name +
            " (incompatible libcuda.so.1 binary layout)");
      }
      realFns[i] = *result;
      return;
    }
  }
  throw std::runtime_error(
      std::string("Too many versions for function: ") + name +
      " (increase MAX_VERSIONS)");
}

#define CREATE_PATCH(fn)                    \
  if (symbol == #fn) {                      \
    doPatch(#fn, real_##fn, *pfn, ps_##fn); \
    return r;                               \
  }

// cuGetProcAddress intercepts: when the runtime asks the driver for a function
// pointer, we patch the returned pointer to point to our mock instead.
template <int N>
CUresult p_cuGetProcAddress(
    const char* symbol_,
    void** pfn,
    int cudaVersion,
    uint64_t flags,
    void* symbolStatus);

template <int N>
CUresult p_cuGetProcAddress_v2(
    const char* symbol_,
    void** pfn,
    int cudaVersion,
    uint64_t flags,
    void* symbolStatus) {
  auto r = ((decltype(&p_cuGetProcAddress_v2<N>))real_cuGetProcAddress_v2[N])(
      symbol_, pfn, cudaVersion, flags, symbolStatus);
  std::string symbol = symbol_;
  FORALL_FUNCTIONS(CREATE_PATCH)
  return r;
}

template <int N>
CUresult p_cuGetProcAddress(
    const char* symbol_,
    void** pfn,
    int cudaVersion,
    uint64_t flags,
    void* symbolStatus) {
  auto r = ((decltype(&p_cuGetProcAddress<N>))real_cuGetProcAddress[N])(
      symbol_, pfn, cudaVersion, flags, symbolStatus);
  std::string symbol = symbol_;
  FORALL_FUNCTIONS(CREATE_PATCH)
  return r;
}

// Instantiate the 4 versioned patch arrays for each function.
#define DEFINE_PATCHES(fn) \
  void* ps_##fn[] = {      \
      (void*)p_##fn<0>,    \
      (void*)p_##fn<1>,    \
      (void*)p_##fn<2>,    \
      (void*)p_##fn<3>,    \
      (void*)p_##fn<4>,    \
      (void*)p_##fn<5>,    \
      (void*)p_##fn<6>,    \
      (void*)p_##fn<7>};

FORALL_FUNCTIONS(DEFINE_PATCHES)

void install() {
  fprintf(stderr, "[mock_cuda] install() starting...\n");
  void* dl = dlopen("libcuda.so.1", RTLD_NOW);
  if (!dl) {
    throw std::runtime_error(
        std::string("Failed to load libcuda.so.1: ") + dlerror());
  }
  fprintf(stderr, "[mock_cuda] libcuda.so.1 loaded at %p\n", dl);

// Required functions — must be direct exports in libcuda.so.1.
#define REDIRECT_FUNCTION(fn)                                            \
  {                                                                      \
    void* sym = dlsym(dl, #fn);                                          \
    if (!sym) {                                                          \
      throw std::runtime_error(std::string("Symbol not found: ") + #fn); \
    }                                                                    \
    doPatch(#fn, real_##fn, sym, ps_##fn);                               \
  }

  FORALL_CORE_FUNCTIONS(REDIRECT_FUNCTION)

// Optional functions — may only be available through cuGetProcAddress.
// If found as direct exports, patch them; otherwise they'll be patched
// lazily when resolved via cuGetProcAddress.
#define TRY_REDIRECT_FUNCTION(fn)            \
  {                                          \
    void* sym = dlsym(dl, #fn);              \
    if (sym) {                               \
      doPatch(#fn, real_##fn, sym, ps_##fn); \
    }                                        \
  }

  FORALL_OPTIONAL_FUNCTIONS(TRY_REDIRECT_FUNCTION)
  fprintf(stderr, "[mock_cuda] install() complete — all functions patched\n");
}

} // namespace

// --- Python API ---

PyObject* enable_mock_cuda(PyObject*, PyObject*) {
  mockCudaEnabled = true;
  Py_RETURN_NONE;
}

PyObject* disable_mock_cuda(PyObject*, PyObject*) {
  mockCudaEnabled = false;
  Py_RETURN_NONE;
}

PyObject* patch_mock_cuda(PyObject*, PyObject*) {
  try {
    install();
    Py_RETURN_NONE;
  } catch (const std::runtime_error& e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
    return nullptr;
  } catch (const std::exception& e) {
    PyErr_SetString(PyExc_Exception, e.what());
    return nullptr;
  } catch (...) {
    PyErr_SetString(PyExc_Exception, "Unknown error during CUDA patching");
    return nullptr;
  }
}
