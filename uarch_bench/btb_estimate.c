// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "utils.h"

// Architecture-specific NOPs - both use 4-byte instructions
#if defined(__aarch64__)
#define ARCH_NOOP __asm__ __volatile__("nop\n\t"); // ARM64 NOP (4 bytes)
#elif defined(__x86_64__)
// x86-64 4-byte NOP: 0F 1F 40 00 (NOP DWORD PTR [RAX+0])
#define ARCH_NOOP __asm__ __volatile__(".byte 0x0F, 0x1F, 0x40, 0x00\n\t");
#else
#error "Unsupported architecture"
#endif

// Build up NOPs hierarchically
#define ARCH_NOOP_2 ARCH_NOOP ARCH_NOOP
#define ARCH_NOOP_4 ARCH_NOOP_2 ARCH_NOOP_2
#define ARCH_NOOP_8 ARCH_NOOP_4 ARCH_NOOP_4
#define ARCH_NOOP_16 ARCH_NOOP_8 ARCH_NOOP_8
#define ARCH_NOOP_32 ARCH_NOOP_16 ARCH_NOOP_16
#define ARCH_NOOP_64 ARCH_NOOP_32 ARCH_NOOP_32
#define ARCH_NOOP_128 ARCH_NOOP_64 ARCH_NOOP_64
#define ARCH_NOOP_256 ARCH_NOOP_128 ARCH_NOOP_128
#define ARCH_NOOP_512 ARCH_NOOP_256 ARCH_NOOP_256
#define ARCH_NOOP_1024 ARCH_NOOP_512 ARCH_NOOP_512

// 1023 = 512 + 256 + 128 + 64 + 32 + 16 + 8 + 4 + 2 + 1
#define ARCH_NOOP_1023                                                \
  ARCH_NOOP_512 ARCH_NOOP_256 ARCH_NOOP_128 ARCH_NOOP_64 ARCH_NOOP_32 \
      ARCH_NOOP_16 ARCH_NOOP_8 ARCH_NOOP_4 ARCH_NOOP_2 ARCH_NOOP

// Function that processes the buffer
long long
process_buffer(int* buffer, int size, int iterations, PerfCounters* perf) {
  long long sum = 0;

  // Warm-up: traverse 10 times and compute sum
  for (int iter = 0; iter < 10; iter++) {
    for (int i = 0; i < size; i++) {
      sum += buffer[i];
    }
  }

  // Start measuring
  perf_counters_enable(perf);

  // Main measurement loop
  for (int iter = 0; iter < iterations; iter++) {
    for (int i = 0; i < size; i++) {
      if (buffer[i] == 0) {
        // Execute 1024 NOPs
        ARCH_NOOP_1024
      } else {
        // Execute 1023 NOPs
        ARCH_NOOP_1023
      }
    }
  }

  // Stop measuring
  perf_counters_disable_and_read(perf);

  return sum / 10;
}

int main() {
  int sizes[] = {
      256,
      512,
      1024,
      4096,
      6144,
      8192,
      10240,
      12288,
      16384,
      32768,
      65536,
      131072,
      262144};
  int iterations[] = {
      100000,
      100000,
      10000,
      10000,
      10000,
      1000,
      10000,
      1000,
      1000,
      1000,
      500,
      500,
      500};
  int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  // Initialize perf counters
  PerfCounters perf;
  if (perf_counters_init(&perf, COUNTER_SET_BRANCH, 0) != 0) {
    fprintf(stderr, "Failed to initialize performance counters\n");
    return 1;
  }

  // Seed random number generator
  srand(42699642);

  printf("BTB Size Estimation\n");
  printf("===================\n\n");
  printf(
      "%-10s %-12s %-15s %-15s %-15s %-15s %-15s\n",
      "Size",
      "Warmup Sum",
      "Instructions",
      "Branch Misses",
      "Misses/Instr",
      "Misses/Iter",
      "Misses/Buffer Size");
  printf(
      "---------------------------------------------------------------------------------------------\n");

  for (int i = 0; i < num_sizes; i++) {
    int size = sizes[i];

    // Allocate and initialize buffer with random 0s and 1s
    int* buffer = (int*)malloc(size * sizeof(int));
    if (!buffer) {
      fprintf(stderr, "Memory allocation failed\n");
      perf_counters_cleanup(&perf);
      return 1;
    }

    for (int j = 0; j < size; j++) {
      buffer[j] = rand() % 2; // Random 0 or 1
    }

    // Process buffer and measure
    long long warmup_sum = process_buffer(buffer, size, iterations[i], &perf);

    double misses_per_instruction =
        (double)perf.count_extra / perf.count_instructions;
    double misses_per_iteration = (double)perf.count_extra / iterations[i];
    double misses_per_buffer_size =
        (double)perf.count_extra / iterations[i] / size;

    printf(
        "%-10d %-12lld %-15lld %-15lld %-15.6f %-15.2f %-15.2f\n",
        size,
        warmup_sum,
        perf.count_instructions,
        perf.count_extra,
        misses_per_instruction,
        misses_per_iteration,
        misses_per_buffer_size);

    free(buffer);
  }

  perf_counters_cleanup(&perf);

  return 0;
}
