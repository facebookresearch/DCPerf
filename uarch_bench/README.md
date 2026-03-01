# Microarchitecture Benchmark Suite

This directory contains a suite of microbenchmarks designed to measure CPU microarchitectural properties, with a focus on instruction frontend (fetch/decode) behavior, cache hierarchies, and branch prediction characteristics.

## Overview

These benchmarks help characterize processor behavior by executing synthetic workloads and collecting performance counter data via Linux perf. The suite supports multiple architectures (x86-64, ARM64). One microbenchmarks is specific to NVIDIA GPUs
and measures performance counters for kernel launches.

## Microbenchmarks

### 1. frontend_study

**File:** `frontend_study.c`

**Purpose:** Measure instruction frontend performance and cache behavior with configurable code layout.

**What it measures:**
- iTLB misses (Instruction Translation Lookaside Buffer)
- L1I cache misses (Level 1 Instruction cache)
- Branch prediction misses
- L2 cache loads
- Cycles and instructions executed

**Key features:**
- Creates multiple dynamically allocated memory regions containing function copies
- Functions are filled with architecture-specific NOPs (4-byte instructions)
- Adjustable function sizes: 16, 64, 256, 1024, 4096, 8192 bytes
- Two access patterns:
  - **Sequential**: Calls functions in order modulo divisor
  - **Random**: Uses PRNG for random function selection

**Command-line parameters:**
```
-d <divisor>          : Number of different functions to cycle through
-i <iterations>       : Total iterations of measurement loop
-b <buffer_size_MB>   : Size of allocated memory regions (MB)
-n <num_buffers>      : Number of separate memory regions
-s <page_KB>          : Page size for function alignment (KB)
-f <func_nops>        : Function size in NOPs (16/64/256/1024/4096/8192)
-r <random_jumps>     : 0=sequential, 1=random function selection
```

**Example usage:**
```bash
./frontend_study -d 10 -i 1000000 -b 32 -n 256 -s 64 -f 1024 -r 0
```

### 2. instr_throughput

**File:** `instr_throughput.c`

**Purpose:** Measure instruction fetch and decode throughput across varying code size ranges.

**What it measures:**
- Cycles and instructions for code execution
- L1I cache misses
- iTLB misses
- L2 cache loads
- DRAM reads (LL cache misses)
- Bytes per cycle (throughput metric)

**Key features:**
- Tests 19 different code sizes from 1KB to 1MB
- Dynamically generates executable code buffers filled with NOP instructions
- Per-iteration metric collection
- Auto-scales iterations based on code size (larger code = fewer iterations)

**Typical test sizes:** 1K, 4K, 8K, 16K, 32K, 64K, 128K, 192K, 256K, 512K, 1M

**Output:** Throughput metrics per iteration for each code size

### 3. btb_estimate

**File:** `btb_estimate.c`

**Purpose:** Estimate Branch Target Buffer (BTB) capacity and behavior by measuring branch prediction performance.

**What it measures:**
- Branch misses
- Instructions executed
- Misses per instruction, per iteration, per buffer entry

**Key features:**
- Tests with buffer sizes from 256 to 262,144 entries (13 sizes)
- Buffer contains random 0s and 1s
- Based on buffer value, executes 1023 or 1024 NOPs (single-bit difference)
- Tracks when branch prediction fails
- Helps determine BTB capacity on the processor

**Logic:** Larger buffer sizes that cause more misses indicate exceeding BTB capacity.

### 4. btb_estimate_calls

**File:** `btb_estimate_calls.c`

**Purpose:** Estimate BTB capacity using indirect function calls instead of conditional branches.

**What it measures:**
- Branch misses from call instructions
- Instructions executed
- Misses per iteration and per function pointer

**Key features:**
- Allocates 128 to 4096 function pointers
- Each function pointer stored on a separate 64KB page
- All functions are identical (1024 NOPs + return)
- Measures branch prediction misses when calling through these pointers
- Random offsets within pages prevent trivial prediction

**Pages tested:** 128 to 4096 pages (21 sizes)

**Output:** Branch miss metrics as function count increases (identifies BTB saturation point)

### 5. fe_study_cuda

**File:** `fe_study_cuda.cu`

**Purpose:** Study instruction frontend behavior on NVIDIA GPUs.

**What it measures:**
- GPU cycles and instructions
- L1I cache misses on GPU
- Function-specific overhead measurement
- GPU instruction issue patterns

**Features:**
- CUDA kernel compilation (sm_90 for x86-64, sm_100 for ARM)
- NOP-based synthetic workloads
- Profiles flush overhead for L1I cache

**Target architecture:** NVIDIA GPUs

## Supporting Files

### utils.c / utils.h

Provides common functionality for all benchmarks:
- **Perf counter abstraction**: Wraps Linux `perf_event_open` syscall
- **Counter sets**: iTLB, L1I, Branch, L2, DRAM reads
- **CPU frequency detection**: Reads from `/sys/devices/system/cpu/` or `/proc/cpuinfo`
- **Measurement results aggregation**: Structures for storing multi-counter data
- **LCG random number generator**: Deterministic RNG (`my_rand()`)

### run_benchmark.sh

Test harness for batch execution:
- Reads input configuration from a file
- Executes benchmarks with multiple parameter combinations
- Outputs results in CSV format

### full_run_input.txt

Sample configuration parameters for `frontend_study`:
- Tests varying divisors (16-512), iterations (10M-100M)
- Memory configurations: 1-512MB buffers with 64KB pages
- Function sizes: 16-8192 NOPs

**Example row:** `10000000,100000000,32,256,64,16,0`
- Divisor=10M, Iterations=100M, Buffer=32MB, Buffers=256, Page=64KB, Function=16 NOPs, No random

## Building

```bash
make
```

The Makefile creates these executables:
1. `frontend_study` - CPU frontend study (gcc)
2. `fe_study_cuda` - CUDA version (nvcc)
3. `instr_throughput` - Throughput benchmark
4. `btb_estimate` - BTB sizing (branches)
5. `btb_estimate_calls` - BTB sizing (calls)

Build configuration:
- Uses `gcc` for C benchmarks with `-O2` optimization
- Uses `nvcc` for CUDA benchmarks with architecture-specific targets
- Supports both x86-64 and ARM64 architectures

## Usage Examples

**Individual benchmark:**
```bash
./frontend_study -d 10 -i 1000000 -b 32 -n 256 -s 64 -f 1024 -r 0
```

**Batch execution with script:**
```bash
./run_benchmark.sh full_run_input.txt > results.csv
```

**Run instruction throughput tests:**
```bash
./instr_throughput
```

**Estimate BTB capacity:**
```bash
./btb_estimate
./btb_estimate_calls
```

## Measurement Focus Areas

| Benchmark | Primary Focus | Architecture | Key Metrics |
|-----------|--------------|--------------|-------------|
| frontend_study | Frontend/cache behavior | x86-64, ARM64 | iTLB, L1I, L2, Branch misses |
| instr_throughput | Code size scaling | x86-64, ARM64 | Throughput, cache misses |
| btb_estimate | BTB capacity (branches) | x86-64, ARM64 | Branch misses vs buffer size |
| btb_estimate_calls | BTB capacity (calls) | x86-64, ARM64 | Call misses vs function count |
| ARM BTB events | ARM64 | fe_study_cuda | GPU frontend | CUDA | GPU-specific patterns |

## Performance Counter Requirements

These benchmarks require access to Linux perf events. You may need to adjust perf_event_paranoid settings:

```bash
# Check current setting
cat /proc/sys/kernel/perf_event_paranoid

# Allow user access to performance counters (may require sudo)
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

## Use Cases

This benchmark suite is useful for:
- Characterizing CPU microarchitecture behavior
- Identifying performance bottlenecks in instruction fetch and decode
- Understanding cache hierarchy characteristics
- Measuring branch prediction capabilities and BTB capacity
- Comparing performance across different processor generations
- GPU instruction frontend analysis
