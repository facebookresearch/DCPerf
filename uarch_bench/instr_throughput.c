// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "utils.h"

// Architecture-specific code generation
#if defined(__aarch64__)
#define NOP_INSTRUCTION 0xD503201F // ARM64 NOP instruction
#define RET_INSTRUCTION 0xD65F03C0 // ARM64 RET instruction
#define INSTRUCTION_SIZE 4
#elif defined(__x86_64__)
// x86-64 4-byte NOP: 0F 1F 40 00 (NOP DWORD PTR [RAX+0])
// Little-endian representation: bytes 0x0F, 0x1F, 0x40, 0x00 -> uint32_t
// 0x00401F0F
#define NOP_INSTRUCTION 0x00401F0F // x86-64 4-byte NOP instruction
#define RET_INSTRUCTION 0xC3 // x86-64 RET instruction (1 byte)
#define INSTRUCTION_SIZE 4
#else
#error "Unsupported architecture"
#endif

// Structure to hold an executable code buffer
typedef struct {
  void* buffer;
  size_t size;
  void (*func)(void);
} CodeBuffer;

// Structure to hold results for a single test
typedef struct {
  const char* size_name;
  unsigned long size_kb;
  unsigned long num_nops;
  CodeBuffer* code;
  long long cycles;
  long long instructions;
  long long l1i_misses;
  long long itlb_misses;
  long long l2_loads;
  long long dram_reads;
  double bytes_per_cycle;
} TestResult;

// Allocate executable memory buffer
CodeBuffer* create_code_buffer(size_t size_kb) {
  CodeBuffer* code = (CodeBuffer*)malloc(sizeof(CodeBuffer));
  if (!code) {
    fprintf(stderr, "Failed to allocate CodeBuffer structure\n");
    return NULL;
  }

  size_t size_bytes = size_kb * 1024;
  code->size = size_bytes;

  // Allocate executable memory using mmap
  code->buffer = mmap(
      NULL,
      size_bytes,
      PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS,
      -1,
      0);

  if (code->buffer == MAP_FAILED) {
    fprintf(
        stderr,
        "Failed to allocate executable memory of size %zu KB\n",
        size_kb);
    free(code);
    return NULL;
  }

  code->func = (void (*)(void))code->buffer;
  return code;
}

// Free code buffer
void free_code_buffer(CodeBuffer* code) {
  if (code) {
    if (code->buffer != MAP_FAILED) {
      munmap(code->buffer, code->size);
    }
    free(code);
  }
}

// Generate executable code in buffer
// Returns the number of NOP instructions generated
unsigned long generate_code(CodeBuffer* code) {
  if (!code || !code->buffer) {
    return 0;
  }

#if defined(__aarch64__)
  // ARM64: Generate code
  uint32_t* ptr = (uint32_t*)code->buffer;
  size_t num_instructions = code->size / sizeof(uint32_t);

  // Fill buffer with NOPs
  for (size_t i = 0; i < num_instructions - 1; i++) {
    ptr[i] = NOP_INSTRUCTION;
  }

  // Add RET at the end
  ptr[num_instructions - 1] = RET_INSTRUCTION;

  // Flush instruction cache for ARM
  __builtin___clear_cache(
      (char*)code->buffer, (char*)code->buffer + code->size);

  return num_instructions - 1; // All instructions except RET are NOPs

#elif defined(__x86_64__)
  // x86-64: Generate code with 4-byte NOPs
  uint32_t* ptr = (uint32_t*)code->buffer;
  size_t num_instructions = code->size / sizeof(uint32_t);

  // Fill buffer with 4-byte NOPs
  for (size_t i = 0; i < num_instructions - 1; i++) {
    ptr[i] = NOP_INSTRUCTION;
  }

  // Add RET at the end (overwrite last NOP with RET in first byte)
  uint8_t* ret_ptr = (uint8_t*)&ptr[num_instructions - 1];
  ret_ptr[0] = RET_INSTRUCTION;

  // x86 typically has coherent I-cache, but flush anyway
  __builtin___clear_cache(
      (char*)code->buffer, (char*)code->buffer + code->size);

  return num_instructions - 1; // All instructions except last are NOPs

#endif
}

// Run a single test with all counter sets
void run_test(TestResult* result, unsigned long iterations) {
  PerfCounters perf;

  if (!result->code || !result->code->func) {
    fprintf(stderr, "Invalid code buffer for test %s\n", result->size_name);
    return;
  }

  // Measure with L1I counter set
  if (perf_counters_init(&perf, COUNTER_SET_L1I, 0) == 0) {
    perf_counters_enable(&perf);
    for (unsigned long i = 0; i < iterations; i++) {
      result->code->func();
    }
    perf_counters_disable_and_read(&perf);
    result->cycles = perf.count_cycles;
    result->instructions = perf.count_instructions;
    result->l1i_misses = perf.count_extra;
    perf_counters_cleanup(&perf);
  }

  // Measure with iTLB counter set
  if (perf_counters_init(&perf, COUNTER_SET_ITLB, 0) == 0) {
    perf_counters_enable(&perf);
    for (unsigned long i = 0; i < iterations; i++) {
      result->code->func();
    }
    perf_counters_disable_and_read(&perf);
    result->itlb_misses = perf.count_extra;
    perf_counters_cleanup(&perf);
  }

  // Measure with L2 counter set
  if (perf_counters_init(&perf, COUNTER_SET_L2, 0) == 0) {
    perf_counters_enable(&perf);
    for (unsigned long i = 0; i < iterations; i++) {
      result->code->func();
    }
    perf_counters_disable_and_read(&perf);
    result->l2_loads = perf.count_extra;
    perf_counters_cleanup(&perf);
  }

  // Measure with DRAM reads counter set
  if (perf_counters_init(&perf, COUNTER_SET_DRAM_READS, 0) == 0) {
    perf_counters_enable(&perf);
    for (unsigned long i = 0; i < iterations; i++) {
      result->code->func();
    }
    perf_counters_disable_and_read(&perf);
    result->dram_reads = perf.count_extra;
    perf_counters_cleanup(&perf);
  }

  // Calculate bytes per cycle (instructions * instruction_size / cycles)
  if (result->cycles > 0) {
    result->bytes_per_cycle =
        ((double)result->instructions * INSTRUCTION_SIZE) /
        (double)result->cycles;
  } else {
    result->bytes_per_cycle = 0.0;
  }
}

// Print results in a table format
void print_results_header() {
  printf("\n");
  printf(
      "====================================================================================================\n");
  printf(
      "%-12s %10s %12s %12s %10s %12s %12s %12s %12s\n",
      "Size",
      "NOPs",
      "Cycles",
      "Instructions",
      "Bytes/Cycle",
      "L1I Misses",
      "iTLB Misses",
      "L2 Loads",
      "DRAM Reads");
  printf(
      "====================================================================================================\n");
}

void print_result(const TestResult* result, unsigned long iterations) {
  // Calculate per-iteration metrics
  double cycles_per_iter = (double)result->cycles / iterations;
  double instructions_per_iter = (double)result->instructions / iterations;
  double l1i_per_iter = (double)result->l1i_misses / iterations;
  double itlb_per_iter = (double)result->itlb_misses / iterations;
  double l2_per_iter = (double)result->l2_loads / iterations;
  double dram_per_iter = (double)result->dram_reads / iterations;

  printf(
      "%-12s %10lu %12.0f %12.0f %10.4f %12.2f %12.2f %12.2f %12.2f\n",
      result->size_name,
      result->num_nops,
      cycles_per_iter,
      instructions_per_iter,
      result->bytes_per_cycle,
      l1i_per_iter,
      itlb_per_iter,
      l2_per_iter,
      dram_per_iter);
}

void print_usage(const char* program_name) {
  fprintf(
      stderr,
      "Usage: %s [-i <iterations>] [-s <specific_size_kb>]\n",
      program_name);
  fprintf(
      stderr,
      "  -i <iterations>: Number of iterations per test (default: auto-calculated, multiples of 10)\n");
  fprintf(
      stderr,
      "  -s <size_kb>: Test only specific size in KB (1, 4, 8, 16, 32, 64, 128, 192, 256, 512, 1024, 4096, 16384, 32768, 65536, 196608, 262144, 524288, 1048576)\n");
  fprintf(stderr, "  -h: Show this help message\n");
}

int main(int argc, char* argv[]) {
  unsigned long iterations = 0; // 0 means auto-calculate
  long specific_size_kb = -1; // -1 means run all sizes

  // Parse command-line arguments
  int opt;
  while ((opt = getopt(argc, argv, "i:s:h")) != -1) {
    switch (opt) {
      case 'i':
        iterations = strtoul(optarg, NULL, 10);
        break;
      case 's':
        specific_size_kb = strtol(optarg, NULL, 10);
        break;
      case 'h':
        print_usage(argv[0]);
        return 0;
      default:
        print_usage(argv[0]);
        return 1;
    }
  }

  // Define test sizes in KB
  unsigned long test_sizes_kb[] = {
      1,
      4,
      8,
      16,
      32,
      64,
      128,
      192,
      256,
      512,
      1024,
      4096,
      16384,
      32768,
      65536,
      196608,
      262144,
      524288,
      1048576};
  int num_sizes = sizeof(test_sizes_kb) / sizeof(test_sizes_kb[0]);

  printf("Dynamic Instruction Throughput Benchmark\n");
  printf("Architecture: ");
#if defined(__aarch64__) || defined(__arm__)
  printf("ARM/AArch64\n");
  printf("Instruction size: 4 bytes (NOP)\n");
#elif defined(__x86_64__) || defined(__i386__)
  printf("x86/x86_64\n");
  printf("Instruction size: 4 bytes (multi-byte NOP)\n");
#else
  printf("Unknown\n");
#endif

  unsigned long cpu_freq = get_cpu_frequency_hz();
  printf("CPU Frequency: %.2f GHz\n", (double)cpu_freq / 1e9);

  // Allocate and generate code buffers
  printf("\nAllocating and generating code buffers...\n");
  TestResult* tests = (TestResult*)calloc(num_sizes, sizeof(TestResult));
  if (!tests) {
    fprintf(stderr, "Failed to allocate test results array\n");
    return 1;
  }

  int num_tests = 0;
  for (int i = 0; i < num_sizes; i++) {
    unsigned long size_kb = test_sizes_kb[i];

    // Skip if specific size requested and this isn't it
    if (specific_size_kb != -1 && size_kb != (unsigned long)specific_size_kb) {
      continue;
    }

    printf("  Creating %lu KB code buffer...\n", size_kb);

    CodeBuffer* code = create_code_buffer(size_kb);
    if (!code) {
      fprintf(stderr, "Failed to create %lu KB buffer, skipping\n", size_kb);
      continue;
    }

    unsigned long num_nops = generate_code(code);

    // Create size name
    char* size_name = (char*)malloc(32);
    if (size_kb >= 1024) {
      snprintf(size_name, 32, "%luM", size_kb / 1024);
    } else {
      snprintf(size_name, 32, "%luK", size_kb);
    }

    tests[num_tests].size_name = size_name;
    tests[num_tests].size_kb = size_kb;
    tests[num_tests].num_nops = num_nops;
    tests[num_tests].code = code;
    tests[num_tests].cycles = 0;
    tests[num_tests].instructions = 0;
    tests[num_tests].l1i_misses = 0;
    tests[num_tests].itlb_misses = 0;
    tests[num_tests].l2_loads = 0;
    tests[num_tests].dram_reads = 0;
    tests[num_tests].bytes_per_cycle = 0.0;

    num_tests++;
  }

  printf("\nRunning tests...\n");
  print_results_header();

  // Run tests
  for (int i = 0; i < num_tests; i++) {
    // Auto-calculate iterations if not specified
    // Use fewer iterations for larger functions to keep runtime reasonable
    // Ensure iterations are multiples of 10
    unsigned long test_iterations = iterations;
    if (test_iterations == 0) {
      if (tests[i].size_kb <= 64) {
        test_iterations = 1000000;
      } else if (tests[i].size_kb <= 512) {
        test_iterations = 100000;
      } else if (tests[i].size_kb <= 4096) {
        test_iterations = 10000;
      } else if (tests[i].size_kb <= 65536) {
        test_iterations = 1000;
      } else {
        test_iterations = 100;
      }

      // Ensure it's a multiple of 10
      test_iterations = (test_iterations / 10) * 10;
      if (test_iterations == 0) {
        test_iterations = 10;
      }
    }

    run_test(&tests[i], test_iterations);
    print_result(&tests[i], test_iterations);
  }

  printf(
      "====================================================================================================\n");
  printf("\nNote: All metrics shown are per-iteration averages.\n");

  // Cleanup
  for (int i = 0; i < num_tests; i++) {
    free_code_buffer(tests[i].code);
    free((void*)tests[i].size_name);
  }
  free(tests);

  return 0;
}
