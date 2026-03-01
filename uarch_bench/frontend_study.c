// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "utils.h"

#define MISS1                 \
  funcIndex = iter % divisor; \
  iter++;                     \
  functionPointers[funcIndex]();
#define MISS4 MISS1 MISS1 MISS1 MISS1
#define MISS16 MISS4 MISS4 MISS4 MISS4
#define MISS64 MISS16 MISS16 MISS16 MISS16
#define MISS256 MISS64 MISS64 MISS64 MISS64
#define MISS1024 MISS256 MISS256 MISS256 MISS256

#define RAND_MISS1                 \
  funcIndex = my_rand() % divisor; \
  iter++;                          \
  functionPointers[funcIndex]();
#define RAND_MISS4 RAND_MISS1 RAND_MISS1 RAND_MISS1 RAND_MISS1
#define RAND_MISS16 RAND_MISS4 RAND_MISS4 RAND_MISS4 RAND_MISS4
#define RAND_MISS64 RAND_MISS16 RAND_MISS16 RAND_MISS16 RAND_MISS16
#define RAND_MISS256 RAND_MISS64 RAND_MISS64 RAND_MISS64 RAND_MISS64
#define RAND_MISS1024 RAND_MISS256 RAND_MISS256 RAND_MISS256 RAND_MISS256

// Architecture-specific NOPs - both use 4-byte instructions
#if defined(__aarch64__)
#define ARCH_NOOP __asm__ __volatile__("nop\n\t"); // ARM64 NOP (4 bytes)
#elif defined(__x86_64__)
// x86-64 4-byte NOP: 0F 1F 40 00 (NOP DWORD PTR [RAX+0])
#define ARCH_NOOP __asm__ __volatile__(".byte 0x0F, 0x1F, 0x40, 0x00\n\t");
#else
#error "Unsupported architecture"
#endif

#define ARCH_NOOP_4 ARCH_NOOP ARCH_NOOP ARCH_NOOP ARCH_NOOP
#define ARCH_NOOP_16 ARCH_NOOP_4 ARCH_NOOP_4 ARCH_NOOP_4 ARCH_NOOP_4
#define ARCH_NOOP_64 ARCH_NOOP_16 ARCH_NOOP_16 ARCH_NOOP_16 ARCH_NOOP_16
#define ARCH_NOOP_256 ARCH_NOOP_64 ARCH_NOOP_64 ARCH_NOOP_64 ARCH_NOOP_64
#define ARCH_NOOP_1024 ARCH_NOOP_256 ARCH_NOOP_256 ARCH_NOOP_256 ARCH_NOOP_256
#define ARCH_NOOP_4096 \
  ARCH_NOOP_1024 ARCH_NOOP_1024 ARCH_NOOP_1024 ARCH_NOOP_1024
#define ARCH_NOOP_8192 ARCH_NOOP_4096 ARCH_NOOP_4096

// Simple no-op functions of various sizes
__attribute__((noinline)) void targetFunction16() {
  ARCH_NOOP_16
}

__attribute__((noinline)) void targetFunction64() {
  ARCH_NOOP_64
}

__attribute__((noinline)) void targetFunction256() {
  ARCH_NOOP_256
}

__attribute__((noinline)) void targetFunction1024() {
  ARCH_NOOP_1024
}

__attribute__((noinline)) void targetFunction4096() {
  ARCH_NOOP_4096
}

__attribute__((noinline)) void targetFunction8192() {
  ARCH_NOOP_8192
}

// Marker function for size estimation
__attribute__((noinline)) void targetFunctionEnd() {}

// Run the main measurement loop
void run_measurement_loop(
    void (**functionPointers)(void),
    int divisor,
    unsigned long long iterations,
    int use_random_jumps) {
  size_t funcIndex = 0;
  unsigned long long iter = 0;

  if (use_random_jumps) {
    while (iter < iterations) {
      RAND_MISS1024
    }
  } else {
    while (iter < iterations) {
      MISS1024
    }
  }
}

// Print usage information
void print_usage(const char* program_name) {
  fprintf(
      stderr,
      "Usage: %s -d <divisor> -i <iterations> -b <buffer_size_MB> -n <num_buffers> -s <page_KB> -f <func nops 16/64/256/1024/4096/8192> -r <random_jumps>\n",
      program_name);
}

int main(int argc, char* argv[]) {
  // Initialize variables with invalid values to detect missing arguments
  int divisor = -1;
  unsigned long long iterations = 0;
  int buffer_size_mb = -1;
  int num_buffers = -1;
  int page_kb = -1;
  int use_random_jumps = -1;
  int func_nops = -1;
  const unsigned long code_alignment = 16;

  // Parse command line arguments
  int opt;
  while ((opt = getopt(argc, argv, "d:i:b:n:s:f:r:h")) != -1) {
    switch (opt) {
      case 'd':
        divisor = atoi(optarg);
        break;
      case 'i':
        iterations = atoll(optarg);
        break;
      case 'b':
        buffer_size_mb = atoi(optarg);
        break;
      case 'n':
        num_buffers = atoi(optarg);
        break;
      case 's':
        page_kb = atoi(optarg);
        break;
      case 'f':
        func_nops = atoi(optarg);
        break;
      case 'r':
        use_random_jumps = atoi(optarg);
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  // Validate all required arguments are present
  if (divisor == -1 || iterations == 0 || buffer_size_mb == -1 ||
      num_buffers == -1 || page_kb == -1 || use_random_jumps == -1 ||
      func_nops == -1) {
    fprintf(stderr, "Error: All arguments are required.\n\n");
    print_usage(argv[0]);
    return 1;
  }

  // Validate argument values
  if (divisor <= 0) {
    fprintf(stderr, "Error: Divisor must be a positive integer\n");
    return 1;
  }

  if (iterations <= 0) {
    fprintf(stderr, "Error: Iterations must be a positive integer\n");
    return 1;
  }

  if (buffer_size_mb <= 0) {
    fprintf(stderr, "Error: Buffer size (MB) must be a positive integer\n");
    return 1;
  }

  if (num_buffers <= 0) {
    fprintf(stderr, "Error: Number of buffers must be a positive integer\n");
    return 1;
  }

  if (page_kb <= 0) {
    fprintf(stderr, "Error: Page (KB) must be a positive integer\n");
    return 1;
  }

  if (func_nops != 16 && func_nops != 64 && func_nops != 256 &&
      func_nops != 1024 && func_nops != 4096 && func_nops != 8192) {
    fprintf(stderr, "Error: Function NOPs must be 16/64/256/1024/4096/8192\n");
    return 1;
  }

  if (use_random_jumps != 0 && use_random_jumps != 1) {
    fprintf(stderr, "Error: Random jumps must be 0 or 1\n");
    return 1;
  }

  // Select function to use based on the requested NOP size
  size_t functionSize = 0;
  void (*targetFunction)(void) = ({
    void (*fn)(void) = NULL;
    switch (func_nops) {
      case 16:
        fn = targetFunction16;
        functionSize = (char*)targetFunction64 - (char*)targetFunction16;
        break;
      case 64:
        fn = targetFunction64;
        functionSize = (char*)targetFunction256 - (char*)targetFunction64;
        break;
      case 256:
        fn = targetFunction256;
        functionSize = (char*)targetFunction1024 - (char*)targetFunction256;
        break;
      case 1024:
        fn = targetFunction1024;
        functionSize = (char*)targetFunction4096 - (char*)targetFunction1024;
        break;
      case 4096:
        fn = targetFunction4096;
        functionSize = (char*)targetFunction8192 - (char*)targetFunction4096;
        break;
      case 8192:
        fn = targetFunction8192;
        functionSize = (char*)targetFunctionEnd - (char*)targetFunction8192;
        break;
      default:
        // Fallback to the smallest function if an invalid size is somehow
        // passed
        fn = targetFunction16;
        functionSize = (char*)targetFunction64 - (char*)targetFunction16;
        break;
    }
    fn;
  });

  // Calculate derived values
  unsigned long page_bytes = (unsigned long long)page_kb * 1024;
  unsigned long long num_copies = (unsigned long long)num_buffers *
      (unsigned long long)buffer_size_mb * 1024 / (unsigned long long)page_kb;
  int num_regions = num_buffers;

  printf("Using divisor: %d\n", divisor);
  printf("Using iterations: %llu\n", iterations);
  printf(
      "Allocating %d regions of %d MB each (total %d MB)...\n",
      num_regions,
      buffer_size_mb,
      num_buffers * buffer_size_mb);
  printf("Page: %d KB\n", page_kb);
  printf("Number of function copies: %llu\n", num_copies);
  printf("Estimated function size: %zu bytes\n", functionSize);

  if (num_copies < (unsigned long long)divisor) {
    fprintf(stderr, "Warning: setting 'divisor' to the num_copies value.\n");
    divisor = num_copies;
  }

  // Allocate array to hold pointers to each region
  void** regions = malloc(num_regions * sizeof(void*));
  if (!regions) {
    fprintf(stderr, "Failed to allocate regions array\n");
    return 1;
  }

  // Map each region separately
  const unsigned long long REGION_SIZE =
      (unsigned long long)buffer_size_mb * 1024 * 1024;
  printf("Mapping %d memory regions...\n", num_regions);
  for (int i = 0; i < num_regions; i++) {
    regions[i] = mmap(
        NULL,
        REGION_SIZE,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

    if (regions[i] == MAP_FAILED) {
      fprintf(stderr, "Failed to allocate memory region %d\n", i);
      // Cleanup already allocated regions
      for (int j = 0; j < i; j++) {
        munmap(regions[j], REGION_SIZE);
      }
      free(regions);
      return 1;
    }
  }
  printf("Successfully mapped %d regions\n", num_regions);

  // Allocate array to store function pointers
  void (**functionPointers)(void) = malloc(num_copies * sizeof(void (*)(void)));
  if (!functionPointers) {
    fprintf(stderr, "Failed to allocate function pointer array\n");
    for (int i = 0; i < num_regions; i++) {
      munmap(regions[i], REGION_SIZE);
    }
    free(regions);
    return 1;
  }

  // Copy function code to regions with page
  printf("Copying function code...\n");
  for (unsigned long long i = 0; i < num_copies; i++) {
    unsigned long intra_page_offset = my_rand() %
        ((page_bytes - functionSize - 1) / code_alignment) * code_alignment;

    // Calculate total byte offset
    unsigned long long total_offset = (i * page_bytes) + intra_page_offset;

    // Determine which region and offset within that region
    int region_idx = total_offset / REGION_SIZE;
    unsigned long long region_offset = total_offset % REGION_SIZE;

    // Calculate destination address
    void* dest = (char*)regions[region_idx] + region_offset;

    memcpy(dest, (void*)targetFunction, functionSize);
    functionPointers[i] = (void (*)(void))dest;
  }
  printf("Function copies created: %llu\n", num_copies);

  // Get CPU frequency (just once)
  unsigned long long cpu_freq_hz = get_cpu_frequency_hz();
  printf("CPU Frequency: %.2f GHz\n\n", cpu_freq_hz / 1e9);

  // Structure to hold all measurement results
  MeasurementResults results;
  memset(&results, 0, sizeof(MeasurementResults));

  // ========== Measurement 0: Timing only (no profiling) ==========
  printf("=== Starting Measurement 0: Timing only (no profiling) ===\n");
  struct timespec start_time, end_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  run_measurement_loop(functionPointers, divisor, iterations, use_random_jumps);
  clock_gettime(CLOCK_MONOTONIC, &end_time);

  // Calculate elapsed time in seconds
  double elapsed_sec = (end_time.tv_sec - start_time.tv_sec) +
      (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
  double iterations_per_sec = iterations / elapsed_sec;

  printf("Elapsed time: %.6f seconds\n", elapsed_sec);
  printf("Iterations per second: %.2f M/s\n", iterations_per_sec / 1e6);
  printf("Completed timing measurement\n\n");

  // ========== Measurement 1: iTLB misses ==========
  printf("=== Starting Measurement 1: iTLB misses ===\n");
  PerfCounters perf_itlb;
  if (perf_counters_init(&perf_itlb, COUNTER_SET_ITLB, 0) == 0) {
    perf_counters_enable(&perf_itlb);
    run_measurement_loop(
        functionPointers, divisor, iterations, use_random_jumps);
    perf_counters_disable_and_read(&perf_itlb);
    results.cycles_itlb = perf_itlb.count_cycles;
    results.instructions_itlb = perf_itlb.count_instructions;
    results.itlb_misses = perf_itlb.count_extra;
    perf_counters_cleanup(&perf_itlb);
    printf("Completed iTLB measurement\n\n");
  }

  // ========== Measurement 2: L1I cache misses ==========
  printf("=== Starting Measurement 2: L1I cache misses ===\n");
  PerfCounters perf_l1i;
  if (perf_counters_init(&perf_l1i, COUNTER_SET_L1I, 0) == 0) {
    perf_counters_enable(&perf_l1i);
    run_measurement_loop(
        functionPointers, divisor, iterations, use_random_jumps);
    perf_counters_disable_and_read(&perf_l1i);
    results.cycles_l1i = perf_l1i.count_cycles;
    results.instructions_l1i = perf_l1i.count_instructions;
    results.l1i_misses = perf_l1i.count_extra;
    perf_counters_cleanup(&perf_l1i);
    printf("Completed L1I measurement\n\n");
  }

  // ========== Measurement 3: Branch misses ==========
  printf("=== Starting Measurement 3: Branch misses ===\n");
  PerfCounters perf_branch;
  if (perf_counters_init(&perf_branch, COUNTER_SET_BRANCH, 0) == 0) {
    perf_counters_enable(&perf_branch);
    run_measurement_loop(
        functionPointers, divisor, iterations, use_random_jumps);
    perf_counters_disable_and_read(&perf_branch);
    results.cycles_branch = perf_branch.count_cycles;
    results.instructions_branch = perf_branch.count_instructions;
    results.branch_misses = perf_branch.count_extra;
    perf_counters_cleanup(&perf_branch);
    printf("Completed Branch measurement\n\n");
  }

  // ========== Measurement 4: L2 cache loads ==========
  printf("=== Starting Measurement 4: L2 cache loads ===\n");
  PerfCounters perf_l2;
  if (perf_counters_init(&perf_l2, COUNTER_SET_L2, 0) == 0) {
    perf_counters_enable(&perf_l2);
    run_measurement_loop(
        functionPointers, divisor, iterations, use_random_jumps);
    perf_counters_disable_and_read(&perf_l2);
    results.cycles_l2 = perf_l2.count_cycles;
    results.instructions_l2 = perf_l2.count_instructions;
    results.l2_loads = perf_l2.count_extra;
    perf_counters_cleanup(&perf_l2);
    printf("Completed L2 measurement\n\n");
  }

  // Print all results
  print_measurement_results(&results, iterations);

  // Cleanup
  free(functionPointers);
  for (int i = 0; i < num_regions; i++) {
    munmap(regions[i], REGION_SIZE);
  }
  free(regions);

  return 0;
}
