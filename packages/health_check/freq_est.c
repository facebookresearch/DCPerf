/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * CPU Frequency Estimation Benchmark
 *
 * This benchmark estimates CPU frequency by executing a chain of dependent
 * ADCS (add with carry, setting flags) instructions. Since each ADCS
 * instruction has a latency of 1 cycle on ARM Neoverse cores and the
 * instructions are dependent (via both data and flags), we achieve IPC = 1,
 * allowing us to estimate frequency as: freq = instructions / time.
 * Using ADCS instead of ADD prevents potential CPU instruction fusion.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Number of instructions to execute in the measurement loop
// 10^9 instructions. At ~3.0 GHz, this takes ~0.33 seconds.
#define TOTAL_INSTRUCTIONS 1000000000UL

// Helper to convert timespec to double seconds
double get_time_sec() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == -1) {
    perror("clock_gettime");
    exit(1);
  }
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Pin thread to a specific core
void pin_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
    fprintf(
        stderr,
        "Error: Could not pin to core %d (check if core exists)\n",
        core_id);
    exit(1);
  }
  printf("Successfully pinned to Core %d\n", core_id);
}

int main(int argc, char* argv[]) {
  int target_core = 2; // Default to core 2
  if (argc > 1) {
    char* endptr;
    errno = 0;
    long parsed_core = strtol(argv[1], &endptr, 10);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0') {
      fprintf(stderr, "Error: Invalid core number '%s'\n", argv[1]);
      exit(1);
    }
    if (parsed_core < 0) {
      fprintf(
          stderr, "Error: Core number cannot be negative: %ld\n", parsed_core);
      exit(1);
    }
    target_core = (int)parsed_core;
  }

  // Validate target_core against available cores
  long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (num_cores == -1) {
    perror("sysconf");
    num_cores = 1; // Assume at least 1 core
  }
  if (target_core >= num_cores) {
    fprintf(
        stderr,
        "Warning: Core %d does not exist (only %ld cores available), falling back to core 0\n",
        target_core,
        num_cores);
    target_core = 0;
  }

  printf("Estimating CPU frequency...\n");
  pin_to_core(target_core);

  // Raise thread priority to reduce scheduler noise
  struct sched_param param;
  param.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (param.sched_priority == -1) {
    perror("Warning: sched_get_priority_max failed");
  } else {
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
      // Not fatal - we can still run with normal priority
      perror("Warning: Could not set real-time priority (try running as root)");
    } else {
      printf(
          "Set real-time priority (SCHED_FIFO, priority %d)\n",
          param.sched_priority);
    }
  }

  // Warmup phase (to wake up the core from idle/sleep states)
  // We run a smaller loop just to get the CPU p-state up.
  volatile uint64_t dummy = 0;
  for (int i = 0; i < 100000000; i++)
    dummy += i;

  // --- CRITICAL SECTION ---
  // We use a dependency chain of ADCS instructions.
  // "adcs x0, x0, #1" has a latency of 1 cycle on Neoverse cores.
  // By making the next instruction depend on the previous result (both data
  // and flags), we force 1 instruction per cycle execution (IPC = 1).
  // Using ADCS prevents potential CPU instruction fusion optimizations.
  // Initialize with random value to prevent CPU optimizations
  srand((unsigned int)time(NULL));
  uint64_t start_val = (uint64_t)rand() ^ ((uint64_t)rand() << 32);
  double start_time = get_time_sec();

  // The loop unrolling prevents branch prediction overhead from dominating.
  // We do 100 instructions per loop iteration, so we run the C loop TOTAL/100
  // times.
  uint64_t loop_count = TOTAL_INSTRUCTIONS / 100;

  asm volatile(
      "1: \n\t"
      "subs %0, %0, #1 \n\t" // Decrement loop counter
      // Block of 100 dependent adcs (add with carry, setting flags)
      // Using adcs prevents CPU instruction fusion optimizations
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "adcs %1, %1, xzr \n\t"
      "b.ne 1b \n\t" // Branch if loop counter != 0
      : "+r"(loop_count), "+r"(start_val) // Inputs/Outputs
      :
      : "cc", "memory" // Clobbers
  );

  double end_time = get_time_sec();
  // --- END CRITICAL SECTION ---

  double elapsed = end_time - start_time;
  // We executed TOTAL_INSTRUCTIONS.
  // Since they are dependent, Cycles ~= Instructions.
  double freq_hz = (double)TOTAL_INSTRUCTIONS / elapsed;
  double freq_ghz = freq_hz / 1e9;

  printf("Instructions: %lu\n", TOTAL_INSTRUCTIONS);
  printf("Time elapsed: %.6f seconds\n", elapsed);
  printf("Estimated Frequency: %.4f GHz\n", freq_ghz);

  return 0;
}
