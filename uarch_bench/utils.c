// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#define _GNU_SOURCE
#include "utils.h"
#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// Wrapper for perf_event_open syscall
static long perf_event_open(
    struct perf_event_attr* hw_event,
    pid_t pid,
    int cpu,
    int group_fd,
    unsigned long flags) {
  return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Setup a perf counter (internal helper)
static int setup_perf_counter(uint32_t type, uint64_t config, int group_fd) {
  struct perf_event_attr pe;
  memset(&pe, 0, sizeof(struct perf_event_attr));
  pe.type = type;
  pe.size = sizeof(struct perf_event_attr);
  pe.config = config;
  pe.disabled = 1; // Start disabled
  pe.exclude_kernel = 1;
  pe.exclude_hv = 1;

  int fd = perf_event_open(&pe, 0, -1, group_fd, 0);
  if (fd == -1) {
    fprintf(
        stderr,
        "Error opening perf counter (type=%u, config=%llu): %s\n",
        type,
        (unsigned long long)config,
        strerror(errno));
  }
  return fd;
}

// Initialize perf counters
int perf_counters_init(
    PerfCounters* perf,
    CounterSet counter_set,
    int verbose) {
  perf->cpu_freq_hz = get_cpu_frequency_hz();
  perf->counter_set = counter_set;

  const char* counter_name = "";
  uint64_t extra_config = 0;
  uint32_t extra_type = PERF_TYPE_HARDWARE;

  // Configure the extra counter based on counter set
  switch (counter_set) {
    case COUNTER_SET_ITLB:
      counter_name = "iTLB-load-misses";
      extra_type = PERF_TYPE_HW_CACHE;
      extra_config = (PERF_COUNT_HW_CACHE_ITLB) |
          (PERF_COUNT_HW_CACHE_OP_READ << 8) |
          (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
      break;
    case COUNTER_SET_L1I:
      counter_name = "L1I-cache-load-misses";
      extra_type = PERF_TYPE_HW_CACHE;
      extra_config = (PERF_COUNT_HW_CACHE_L1I) |
          (PERF_COUNT_HW_CACHE_OP_READ << 8) |
          (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
      break;
    case COUNTER_SET_BRANCH:
      counter_name = "branch-misses";
      extra_type = PERF_TYPE_HARDWARE;
      extra_config = PERF_COUNT_HW_BRANCH_MISSES;
      break;
    case COUNTER_SET_L2:
      counter_name = "L2/LL-cache-loads";
      extra_type = PERF_TYPE_HW_CACHE;
      // Note: PERF_COUNT_HW_CACHE_LL refers to Last Level Cache
      // On most ARM systems, this is L2 cache (or L3 if present)
      extra_config = (PERF_COUNT_HW_CACHE_LL) |
          (PERF_COUNT_HW_CACHE_OP_READ << 8) |
          (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
      break;
    case COUNTER_SET_DRAM_READS:
      counter_name = "DRAM-reads";
      extra_type = PERF_TYPE_HW_CACHE;
      // Measure last-level cache misses which indicate DRAM reads
      extra_config = (PERF_COUNT_HW_CACHE_LL) |
          (PERF_COUNT_HW_CACHE_OP_READ << 8) |
          (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
      break;
  }

  if (verbose == 1) {
    printf(
        "Setting up perf counters: cycles, instructions, %s\n", counter_name);
  }

  perf->fd_cycles =
      setup_perf_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1);
  perf->fd_instructions = setup_perf_counter(
      PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, perf->fd_cycles);
  perf->fd_extra =
      setup_perf_counter(extra_type, extra_config, perf->fd_cycles);

  if (perf->fd_cycles == -1 || perf->fd_instructions == -1 ||
      perf->fd_extra == -1) {
    fprintf(
        stderr,
        "Warning: Some perf counters unavailable, continuing without them\n");
    perf->available = 0;
    return -1;
  }

  perf->available = 1;
  perf->count_cycles = 0;
  perf->count_instructions = 0;
  perf->count_extra = 0;
  return 0;
}

// Enable perf counters
void perf_counters_enable(PerfCounters* perf) {
  if (!perf->available) {
    return;
  }

  ioctl(perf->fd_cycles, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
  ioctl(perf->fd_cycles, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

// Disable perf counters and read results
void perf_counters_disable_and_read(PerfCounters* perf) {
  if (!perf->available) {
    return;
  }

  ioctl(perf->fd_cycles, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

  read(perf->fd_cycles, &perf->count_cycles, sizeof(long long));
  read(perf->fd_instructions, &perf->count_instructions, sizeof(long long));
  read(perf->fd_extra, &perf->count_extra, sizeof(long long));
}

// Print all measurement results
void print_measurement_results(
    const MeasurementResults* results,
    unsigned long long iterations) {
  printf("\n=== Measurement Results ===\n");
  printf("Iterations: %llu\n\n", iterations);

  // iTLB measurements
  printf("--- iTLB Measurement ---\n");
  printf("Cycles: %lld\n", results->cycles_itlb);
  printf("Instructions: %lld\n", results->instructions_itlb);
  printf("iTLB Load Misses: %lld\n", results->itlb_misses);
  if (iterations > 0) {
    double itlb_per_iter = (double)results->itlb_misses / (double)iterations;
    double cycles_per_iter = (double)results->cycles_itlb / (double)iterations;
    printf("iTLB Misses / Iteration: %.6f\n", itlb_per_iter);
    printf("Cycles / Iteration: %.6f\n", cycles_per_iter);
  }

  // L1I measurements
  printf("\n--- L1I Cache Measurement ---\n");
  printf("Cycles: %lld\n", results->cycles_l1i);
  printf("Instructions: %lld\n", results->instructions_l1i);
  printf("L1I Cache Load Misses: %lld\n", results->l1i_misses);
  if (iterations > 0) {
    double l1i_per_iter = (double)results->l1i_misses / (double)iterations;
    double cycles_per_iter = (double)results->cycles_l1i / (double)iterations;
    printf("L1I Misses / Iteration: %.6f\n", l1i_per_iter);
    printf("Cycles / Iteration: %.6f\n", cycles_per_iter);
  }

  // Branch measurements
  printf("\n--- Branch Prediction Measurement ---\n");
  printf("Cycles: %lld\n", results->cycles_branch);
  printf("Instructions: %lld\n", results->instructions_branch);
  printf("Branch Misses: %lld\n", results->branch_misses);
  if (iterations > 0) {
    double branch_per_iter =
        (double)results->branch_misses / (double)iterations;
    double cycles_per_iter =
        (double)results->cycles_branch / (double)iterations;
    printf("Branch Misses / Iteration: %.6f\n", branch_per_iter);
    printf("Cycles / Iteration: %.6f\n", cycles_per_iter);
  }

  // L2 Cache measurements
  // Note: ARM cache line size is typically 64 bytes
  // Reference: ARM Architecture Reference Manual, most ARM cores use 64-byte
  // cache lines
  printf("\n--- L2 Cache Measurement ---\n");
  printf("Cycles: %lld\n", results->cycles_l2);
  printf("Instructions: %lld\n", results->instructions_l2);
  printf("L2 Cache Loads: %lld\n", results->l2_loads);
  if (iterations > 0) {
    const long long CACHE_LINE_SIZE = 64; // bytes
    double l2_loads_per_iter = (double)results->l2_loads / (double)iterations;
    double l2_bytes_per_iter = l2_loads_per_iter * CACHE_LINE_SIZE;
    double cycles_per_iter = (double)results->cycles_l2 / (double)iterations;
    printf("L2 Loads / Iteration: %.6f\n", l2_loads_per_iter);
    printf("L2 Bytes Loaded / Iteration: %.2f bytes\n", l2_bytes_per_iter);
    printf("Cycles / Iteration: %.6f\n", cycles_per_iter);
  }
}

// Cleanup perf counters
void perf_counters_cleanup(PerfCounters* perf) {
  if (!perf->available) {
    return;
  }

  close(perf->fd_cycles);
  close(perf->fd_instructions);
  close(perf->fd_extra);
}

// Read CPU frequency in Hz
unsigned long long get_cpu_frequency_hz(void) {
  FILE* fp;
  unsigned long long freq_khz = 0;

  // Try to read from sysfs (frequency in kHz)
  fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
  if (fp) {
    if (fscanf(fp, "%llu", &freq_khz) == 1) {
      fclose(fp);
      return freq_khz * 1000; // Convert kHz to Hz
    }
    fclose(fp);
  }

  // Fallback: try to read from /proc/cpuinfo
  fp = fopen("/proc/cpuinfo", "r");
  if (fp) {
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
      // Look for "cpu MHz" line
      if (strstr(line, "cpu MHz")) {
        float freq_mhz = 0;
        if (sscanf(line, "cpu MHz : %f", &freq_mhz) == 1) {
          fclose(fp);
          return (unsigned long long)(freq_mhz * 1000000); // Convert MHz to Hz
        }
      }
    }
    fclose(fp);
  }

  fprintf(
      stderr, "Warning: Could not read CPU frequency, using default 2.0 GHz\n");
  return 2000000000ULL; // Default to 2 GHz
}

// Simple inline random number generator (LCG)
static unsigned long rand_state = 1; // 42069420;

void my_srand(unsigned long seed) {
  rand_state = seed;
}

unsigned long my_rand(void) {
  rand_state = rand_state * 1103515245 + 12345;
  return (rand_state / 65536) % 32768;
}
