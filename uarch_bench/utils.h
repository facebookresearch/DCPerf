// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#ifndef UTILS_H
#define UTILS_H

// Counter type for different measurement passes
typedef enum {
  COUNTER_SET_ITLB,
  COUNTER_SET_L1I,
  COUNTER_SET_BRANCH,
  COUNTER_SET_L2,
  COUNTER_SET_DRAM_READS
} CounterSet;

// Perf counter management struct
typedef struct {
  int fd_cycles;
  int fd_instructions;
  int fd_extra; // Can be iTLB, L1I, or branch depending on counter set
  int available;
  long long count_cycles;
  long long count_instructions;
  long long count_extra;
  unsigned long long cpu_freq_hz;
  CounterSet counter_set;
} PerfCounters;

// Perf counter functions
int perf_counters_init(PerfCounters* perf, CounterSet counter_set, int verbose);
void perf_counters_enable(PerfCounters* perf);
void perf_counters_disable_and_read(PerfCounters* perf);
void perf_counters_cleanup(PerfCounters* perf);

// Results structure to hold all measurements
typedef struct {
  long long cycles_itlb;
  long long instructions_itlb;
  long long itlb_misses;
  long long cycles_l1i;
  long long instructions_l1i;
  long long l1i_misses;
  long long cycles_branch;
  long long instructions_branch;
  long long branch_misses;
  long long cycles_l2;
  long long instructions_l2;
  long long l2_loads;
} MeasurementResults;

void print_measurement_results(
    const MeasurementResults* results,
    unsigned long long iterations);

// CPU frequency detection
unsigned long long get_cpu_frequency_hz(void);

// Random number generator
void my_srand(unsigned long seed);
unsigned long my_rand(void);

#endif // UTILS_H
