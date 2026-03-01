// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
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

// Template function that executes 1024 NOPs and returns
__attribute__((noinline)) void nop_function_1024() {
  ARCH_NOOP_1024
}

// Marker function to calculate size
__attribute__((noinline)) void nop_function_end() {}

// Function that processes the buffer of function pointers
void process_buffer(
    void (**buffer)(),
    int size,
    int iterations,
    PerfCounters* perf) {
  // Warm-up: traverse 10 times and call functions
  for (int iter = 0; iter < 10; iter++) {
    for (int i = 0; i < size; i++) {
      buffer[i]();
    }
  }

  // Start measuring
  perf_counters_enable(perf);

  // Main measurement loop - call function pointers
  for (int iter = 0; iter < iterations; iter++) {
    for (int i = 0; i < size; i++) {
      buffer[i]();
    }
  }

  // Stop measuring
  perf_counters_disable_and_read(perf);
}

int main(int argc, char* argv[]) {
  // Parse command line arguments
  int iterations = 100000; // Default value
  int max_size = 64; // Default value

  if (argc > 1) {
    iterations = atoi(argv[1]);
    if (iterations <= 0) {
      fprintf(stderr, "Error: iterations must be positive\n");
      fprintf(stderr, "Usage: %s [iterations] [max_size]\n", argv[0]);
      return 1;
    }
  }

  if (argc > 2) {
    max_size = atoi(argv[2]);
    if (max_size <= 0) {
      fprintf(stderr, "Error: max_size must be positive\n");
      fprintf(stderr, "Usage: %s [iterations] [max_size]\n", argv[0]);
      return 1;
    }
  }

  // Dynamically generate sizes array from 1 to max_size
  int* sizes = malloc(max_size * sizeof(int));
  if (!sizes) {
    fprintf(stderr, "Failed to allocate sizes array\n");
    return 1;
  }
  for (int i = 0; i < max_size; i++) {
    sizes[i] = i + 1;
  }
  int num_sizes = max_size;
  const size_t PAGE_SIZE = 2 * 1024 * 1024; // 64KB pages
  const size_t ALIGNMENT = 16; // Function alignment

  // Calculate function size
  size_t function_size = (char*)nop_function_end - (char*)nop_function_1024;
  printf("Function size: %zu bytes\n", function_size);

  if (function_size > PAGE_SIZE) {
    fprintf(stderr, "Error: Function size exceeds page size\n");
    return 1;
  }

  // Initialize perf counters
  PerfCounters perf;
  if (perf_counters_init(&perf, COUNTER_SET_BRANCH, 0) != 0) {
    fprintf(stderr, "Failed to initialize performance counters\n");
    return 1;
  }

  // Seed random number generator
  srand(time(NULL));

  printf("BTB Size Estimation (Function Calls)\n");
  printf("=====================================\n\n");
  printf(
      "%-10s %-10s %-15s %-15s %-15s %-15s\n",
      "Size",
      "Pages",
      "Instructions",
      "Branch Misses",
      "Misses/Iter",
      "Misses/Buffer Size");
  printf(
      "------------------------------------------------------------------------------------------------------------\n");

  for (int i = 0; i < num_sizes; i++) {
    int size = sizes[i];

    // Calculate number of pages needed (one function per page)
    int num_pages = size;

    // Allocate array to hold memory regions
    void** pages = malloc(num_pages * sizeof(void*));
    if (!pages) {
      fprintf(stderr, "Failed to allocate pages array\n");
      perf_counters_cleanup(&perf);
      return 1;
    }

    // Allocate function pointer buffer
    void (**buffer)() = malloc(size * sizeof(void (*)()));
    if (!buffer) {
      fprintf(stderr, "Failed to allocate buffer\n");
      free(pages);
      perf_counters_cleanup(&perf);
      return 1;
    }

    // Allocate executable memory pages and copy functions
    for (int j = 0; j < num_pages; j++) {
      // Allocate executable page
      pages[j] = mmap(
          NULL,
          PAGE_SIZE,
          PROT_READ | PROT_WRITE | PROT_EXEC,
          MAP_PRIVATE | MAP_ANONYMOUS,
          -1,
          0);

      if (pages[j] == MAP_FAILED) {
        fprintf(
            stderr,
            "Failed to allocate executable page %d for size %d\n",
            j,
            size);
        // Cleanup previously allocated pages
        for (int k = 0; k < j; k++) {
          munmap(pages[k], PAGE_SIZE);
        }
        free(buffer);
        free(pages);
        perf_counters_cleanup(&perf);
        return 1;
      }

      // Calculate random offset within page (aligned)
      size_t max_offset = PAGE_SIZE - function_size;
      size_t offset = (rand() % (max_offset / ALIGNMENT)) * ALIGNMENT;

      // Copy function to this offset
      void* dest = (char*)pages[j] + offset;
      memcpy(dest, (void*)nop_function_1024, function_size);

      // Store function pointer in buffer
      buffer[j] = (void (*)())dest;
    }

    // Process buffer and measure
    process_buffer(buffer, size, iterations, &perf);

    double misses_per_iteration = (double)perf.count_extra / iterations;
    double misses_per_buffer_size =
        (double)perf.count_extra / iterations / size;

    printf(
        "%-10d %-10d %-15lld %-15lld %-15.2f %-15.6f\n",
        size,
        num_pages,
        perf.count_instructions,
        perf.count_extra,
        misses_per_iteration,
        misses_per_buffer_size);

    // Cleanup
    for (int j = 0; j < num_pages; j++) {
      munmap(pages[j], PAGE_SIZE);
    }
    free(buffer);
    free(pages);
  }

  free(sizes);
  perf_counters_cleanup(&perf);

  return 0;
}
