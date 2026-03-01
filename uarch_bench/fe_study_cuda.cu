// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#include <stdio.h>

extern "C" {
#include "utils.h"
}

__global__ void emptyKernel(const int n, float* __restrict__ gOutput) {}

// Macros for unrolled NOP instructions (similar to frontend_study.c)
//
// AArch64 Instruction Encoding:
// All AArch64 instructions are fixed-length 32-bit (4 bytes) encodings.
// Reference: ARM Architecture Reference Manual ARMv8, Section C3.1 "A64
// Instruction Set Encoding"
// https://developer.arm.com/documentation/ddi0487/latest (ARM ARM for ARMv8-A)
//
// Calculation: 2MB = 2,097,152 bytes / 4 bytes per instruction = 524,288
// instructions
#define NOOP __asm__ __volatile__("nop\n\t");
#define NOOP_4 NOOP NOOP NOOP NOOP
#define NOOP_16 NOOP_4 NOOP_4 NOOP_4 NOOP_4
#define NOOP_64 NOOP_16 NOOP_16 NOOP_16 NOOP_16
#define NOOP_256 NOOP_64 NOOP_64 NOOP_64 NOOP_64
#define NOOP_1024 NOOP_256 NOOP_256 NOOP_256 NOOP_256
#define NOOP_4096 NOOP_1024 NOOP_1024 NOOP_1024 NOOP_1024
#define NOOP_16384 NOOP_4096 NOOP_4096 NOOP_4096 NOOP_4096
#define NOOP_65536 NOOP_16384 NOOP_16384 NOOP_16384 NOOP_16384
#define NOOP_262144 NOOP_65536 NOOP_65536 NOOP_65536 NOOP_65536
#define NOOP_524288 NOOP_262144 NOOP_262144

// Function to flush instruction cache using NOP instructions

__attribute__((noinline)) void flush_instruction_l1_cache_noop() {
#if defined(__aarch64__)
  // ARM/AArch64: 16,384 NOPs (64KB)
  NOOP_16384
#elif defined(__x86_64__) || defined(__i386__)
  // x86: 24K noops is 24KB
  NOOP_16384
  NOOP_4096
  NOOP_4096
#else
  // Fallback: default to 16,384 NOPs
  NOOP_16384
#endif
}

__attribute__((noinline)) void flush_for_flush_instruction_l1_cache_noop() {
#if defined(__aarch64__)
  // ARM/AArch64: 16,384 NOPs (64KB)
  NOOP_16384
#elif defined(__x86_64__) || defined(__i386__)
  // x86: 8,192 NOPs (32KB)
  NOOP_4096
  NOOP_4096
#else
  // Fallback: default to 16,384 NOPs
  NOOP_16384
#endif
}

// Structure to hold profiled flush overhead metrics
struct FlushOverhead {
  double cycles_per_call;
  double instructions_per_call;
  double l1i_misses_per_call;
};

// Profile the flush_instruction_cache function to measure its overhead
FlushOverhead profile_flush_overhead(CounterSet counter_set) {
  FlushOverhead overhead = {0.0, 0.0, 0.0};

  // Initialize perf counters for the specified counter set
  PerfCounters perf;
  int perf_available = (perf_counters_init(&perf, counter_set, 0) == 0);

  if (!perf_available) {
    printf("Warning: Performance counters not available for flush profiling\n");
    return overhead;
  }

  // Profile provided flush function
  printf("Profiling flush function overhead ...\n");

  // Flush the L1I cache using a different function
  flush_for_flush_instruction_l1_cache_noop();
  perf_counters_enable(&perf);
  flush_instruction_l1_cache_noop();
  perf_counters_disable_and_read(&perf);

  // Calculate per-call overhead
  overhead.cycles_per_call = (double)perf.count_cycles;
  overhead.instructions_per_call = (double)perf.count_instructions;
  overhead.l1i_misses_per_call = (double)perf.count_extra;

  printf(
      "Flush overhead per call: Cycles: %.2f, Instructions: %.2f, Counter: %.2f\n",
      overhead.cycles_per_call,
      overhead.instructions_per_call,
      overhead.l1i_misses_per_call);

  perf_counters_cleanup(&perf);
  return overhead;
}

std::vector<double> timeLaunch(
    const int numReps,
    cudaStream_t stream,
    const std::vector<int>& gridSizes,
    const FlushOverhead& flush_overhead,
    CounterSet counter_set) {
  std::vector<double> timeUs;

  // Initialize perf counters for the specified counter set
  PerfCounters perf;
  int perf_available = (perf_counters_init(&perf, counter_set, 0) == 0);
  if (!perf_available) {
    printf(
        "Warning: Performance counters not available for launch profiling\n");
    return timeUs;
  }

  for (const auto& numBlocks : gridSizes) {
    dim3 block(256);
    dim3 grid(numBlocks);
    cudaDeviceSynchronize();

    // Reset and enable perf counters before measurement
    perf_counters_enable(&perf);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < numReps; ++i) {
      flush_instruction_l1_cache_noop();
      emptyKernel<<<grid, block, 0, stream>>>(numBlocks, nullptr);
    }

    // Disable and read perf counters after measurement
    perf_counters_disable_and_read(&perf);
    cudaDeviceSynchronize();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::micro> elapsed = end - start;
    timeUs.push_back(elapsed.count() / numReps);

    // Calculate per-launch metrics (for emptyKernel only)
    double instructions_per_launch = perf.count_instructions / (double)numReps -
        (double)flush_overhead.instructions_per_call;
    double counter_per_launch = (double)perf.count_extra / (double)numReps -
        (double)flush_overhead.l1i_misses_per_call;
    double cycles_per_launch = (double)perf.count_cycles / (double)numReps;
    double ipc = (perf.count_cycles > 0)
        ? ((double)perf.count_instructions / (double)perf.count_cycles)
        : 0.0;

    flush_instruction_l1_cache_noop();
    flush_instruction_l1_cache_noop();
    flush_instruction_l1_cache_noop();
    perf_counters_enable(&perf);
    emptyKernel<<<grid, block, 0, stream>>>(numBlocks, nullptr);
    perf_counters_disable_and_read(&perf);

    double instructions_per_launch_single = (double)perf.count_instructions;
    double counter_per_launch_single = (double)perf.count_extra;
    double cycles_per_launch_single = (double)perf.count_cycles;
    double ipc_single = (perf.count_cycles > 0)
        ? ((double)perf.count_instructions / (double)perf.count_cycles)
        : 0.0;

    printf(
        "  Grid %6d: Repeat-Run: Instructions: %.2f, Counter: %.2f, Cycles (including overhead): %.2f, IPC: %.4f\n",
        numBlocks,
        instructions_per_launch,
        counter_per_launch,
        cycles_per_launch,
        ipc);

    printf(
        "  Grid %6d: Single-Run: Instructions: %.2f, Counter: %.2f, Cycles (excluding overhead): %.2f, IPC: %.4f\n",
        numBlocks,
        instructions_per_launch_single,
        counter_per_launch_single,
        cycles_per_launch_single,
        ipc_single);
  }

  // Cleanup perf counters
  if (perf_available) {
    perf_counters_cleanup(&perf);
  }

  return timeUs;
}

int main(int argc, char* argv[]) {
  int gpuIdx = 0;
  int numReps = 10000;
  int minBlocks = 1;
  int maxBlocks = 128 * 1024;
  bool warmUpLaunch = false;
  int verbosityLevel = 1;
  CounterSet counter_set = COUNTER_SET_L1I; // Default to L1I cache misses

  if (argc >= 2) {
    gpuIdx = atoi(argv[1]);
  }
  if (argc >= 3) {
    numReps = atoi(argv[2]);
  }
  if (argc >= 5) {
    minBlocks = atoi(argv[3]);
    maxBlocks = atoi(argv[4]);
  }
  if (argc >= 6) {
    warmUpLaunch = (atoi(argv[5]) == 1);
  }
  if (argc >= 7) {
    // Parse counter set argument
    const char* counter_arg = argv[6];
    if (strcmp(counter_arg, "l1i") == 0 || strcmp(counter_arg, "L1I") == 0) {
      counter_set = COUNTER_SET_L1I;
      printf("Using L1 Instruction cache misses counter\n");
    } else if (
        strcmp(counter_arg, "itlb") == 0 || strcmp(counter_arg, "ITLB") == 0) {
      counter_set = COUNTER_SET_ITLB;
      printf("Using iTLB misses counter\n");
    } else if (
        strcmp(counter_arg, "l2") == 0 || strcmp(counter_arg, "L2") == 0) {
      counter_set = COUNTER_SET_L2;
      printf("Using L2 load instructions counter\n");
    } else {
      fprintf(
          stderr,
          "Error: Invalid counter type '%s'. Valid options: l1i, itlb, l2\n",
          counter_arg);
      fprintf(
          stderr,
          "Usage: %s [gpuIdx] [numReps] [minBlocks] [maxBlocks] [warmUpLaunch] [counter_type]\n",
          argv[0]);
      fprintf(
          stderr,
          "  counter_type: l1i (L1I cache misses), itlb (iTLB misses), l2 (L2 load instructions)\n");
      return 1;
    }
  }

  std::vector<int> gridSizes;
  for (int i = minBlocks; i <= maxBlocks; i *= 2) {
    gridSizes.push_back(i);
  }

  cudaSetDevice(gpuIdx);
  cudaFree(0);

  cudaStream_t stream = 0;
  cudaStreamCreate(&stream);

  // Warmp up launch, to get the code to GPU.
  if (warmUpLaunch) {
    emptyKernel<<<1, 1>>>(0, nullptr);
  }

  // Profile selected flush function overhead
  // Calculate the size of the flush function
  void* flush_func_ptr = (void*)flush_instruction_l1_cache_noop;
  void* flush_for_flush_func_ptr =
      (void*)flush_for_flush_instruction_l1_cache_noop;
  size_t flush_func_size =
      (size_t)((char*)flush_for_flush_func_ptr - (char*)flush_func_ptr);
  printf(
      "Size of flush_instruction_l1_cache_noop function: %zu bytes\n",
      flush_func_size);

  FlushOverhead flush_overhead = profile_flush_overhead(counter_set);

  // Measure launch latency for null stream.
  printf("\n=== Measuring null stream launch latencies ===\n");
  auto nullStreamLaunchLatencies =
      timeLaunch(numReps, 0, gridSizes, flush_overhead, counter_set);

  // Measure launch latency for non-null stream.
  printf("\n=== Measuring non-null stream launch latencies ===\n");
  auto nonNullStreamLaunchLatencies =
      timeLaunch(numReps, stream, gridSizes, flush_overhead, counter_set);

  assert(
      nullStreamLaunchLatencies.size() == nonNullStreamLaunchLatencies.size());
  assert(nullStreamLaunchLatencies.size() == gridSizes.size());

  if (verbosityLevel > 0) {
    std::cout << gpuIdx << " " << numReps << " " << minBlocks << " "
              << maxBlocks << " " << warmUpLaunch << std::endl;
    printf("  CTAs   null non-null\n");
    printf("----------------------\n");
  }
  for (long unsigned int i = 0; i < gridSizes.size(); ++i) {
    printf(
        "%6d %6.2f %6.2f\n",
        gridSizes[i],
        nullStreamLaunchLatencies[i],
        nonNullStreamLaunchLatencies[i]);
    // std::cout << gridSizes[i] << " " << nullStreamLaunchLatencies[i] << " "
    //          << nonNullStreamLaunchLatencies[i] << std::endl;
  }

  cudaStreamDestroy(stream);

  return 0;
}
