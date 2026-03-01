# CPU Micro Benchmark (micro_cpu)

CPU microbenchmarking using stress-ng. This benchmark evaluates CPU subsystem performance using a wide range of stressors for HPC CPU qualification, including compute, cache hierarchy, vectorization, and branch prediction workloads.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Running the Benchmark](#running-the-benchmark)
- [Cleanup](#cleanup)
- [Parameter Reference](#parameter-reference)
- [Common Workloads](#common-workloads)
- [Interpreting Results](#interpreting-results)

---

## Prerequisites

- Linux (Ubuntu or CentOS/RHEL)
- Root or sudo access (for CPU governor control)

Verify stress-ng is available:
```bash
stress-ng --version
```

---

## Installation

The install script automatically detects your Linux distribution and installs dependencies:

```bash
# Via Benchpress
sudo ./benchpress_cli.py -b ehw install micro_cpu

# Or manually
sudo ./packages/cdn_bench/micro_cpu/install_cpu_micro.sh
```

**What gets installed:**
- `stress-ng` - System stress testing tool with 300+ stressors
- `cpupower` - CPU frequency scaling control (kernel-tools)
- `numactl` - NUMA topology utilities

---

## Running the Benchmark

### Basic Usage

```bash
# Run with default settings (cpu stressor, all methods, all threads, 60s)
sudo ./benchpress_cli.py -b ehw run micro_cpu
```

### Custom Parameters

Override default parameters using the `-o` flag with the format `"micro_cpu:<args>"`:

```bash
# CPU stressor with specific method
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cpu --cpu-method=matrixprod --timeout=120" run

# Matrix stressor for SIMD/vectorization testing
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=matrix --matrix-size=256 --timeout=60" run

# Cache hierarchy testing
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cache --workers=0 --timeout=120" run

# Vector math stressor
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=vecmath --timeout=60" run

# Full example with multiple parameters
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cpu --cpu-method=all --workers=16 --timeout=120 --governor=performance --verify=1" run
```

---

## Cleanup

Remove artifacts and reset state:

```bash
# Via Benchpress
sudo ./benchpress_cli.py -b ehw clean micro_cpu

# Or manually
sudo ./packages/cdn_bench/micro_cpu/cleanup_cpu_micro.sh
```

---

## Parameter Reference

### Core Configuration

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `stressor` | Stressor type to run | `cpu` | `cpu`, `cache`, `matrix`, `vecmath`, `vecwide`, `bsearch`, `qsort`, `zlib`, `stream` |
| `workers` | Number of worker instances | `0` (auto = nproc) | `1`, `8`, `16`, `32` |
| `timeout` | Test duration (seconds) | `60` | `30`, `60`, `120`, `300` |
| `governor` | CPU frequency governor | `performance` | `performance`, `ondemand`, `powersave` |

### CPU Stressor Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `cpu_method` | CPU stressor method | `all` | `all`, `ackermann`, `bitops`, `callfunc`, `cdouble`, `cfloat`, `matrixprod`, `prime`, `queens`, `fft`, `pi` |

### Matrix Stressor Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `matrix_size` | Matrix dimensions (NxN) | `128` | `64`, `128`, `256`, `512` |

### VM Stressor Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `vm_bytes` | Memory allocation per worker | `256M` | `64M`, `256M`, `1G`, `4G` |

### Advanced Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `taskset` | CPU affinity mask | *(none)* | `0-7`, `0,2,4,6` |
| `verify` | Verify stressor computations | `0` | `0`, `1` |
| `aggressive` | Enable aggressive mode (maximize stress) | `0` | `0`, `1` |

---

## Common Workloads

### 1. General HPC CPU Qualification

Full CPU stressor sweep across all methods - the default for qualifying new CPUs.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cpu --cpu-method=all --timeout=120" run
```

**What to look at:**
- `bogo_ops_per_sec_real_time` - throughput capacity
- `cpu_usage_per_instance` - efficiency per core
- Consistency across methods

### 2. CDN Edge Host

Simulates edge request processing: branchy code, high syscall rate, latency-sensitive.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cpu --cpu-method=callfunc --timeout=60" run
```

**With branch prediction stress:**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=bsearch --timeout=60" run
```

**What to look at:**
- Throughput under function call overhead
- Branch prediction efficiency (via perf hooks)
- Scaling when workers > physical cores

### 3. Cache Hierarchy Testing

Evaluate L1/L2/L3 cache performance and coherence.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cache --timeout=120" run
```

**What to look at:**
- Cache miss rates (via perfstat hook)
- Performance degradation across NUMA boundaries
- Impact of cache size on throughput

### 4. SIMD / Vectorization

Test vector processing capability with matrix and vector math stressors.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=matrix --matrix-size=256 --timeout=60" run
```

**Vector math variant:**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=vecmath --timeout=60" run
```

**What to look at:**
- FLOPS throughput
- SIMD utilization (via perf counters)
- Scaling with matrix size

### 5. Compression Workloads

For storage/networking hosts doing inline compression.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=zlib --timeout=120" run
```

**What to look at:**
- Compression throughput
- Memory bandwidth utilization
- CPU utilization patterns

### 6. Quick System Validation

Fast sanity check before deeper testing:

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--stressor=cpu --cpu-method=matrixprod --timeout=30" run
```

---

## Workload Selection Guide

| System Type | Stressor | Key Parameters | Primary Metric |
|-------------|----------|----------------|----------------|
| HPC qualification | `cpu` | `cpu-method=all`, long timeout | bogo-ops/sec across methods |
| CDN Edge | `cpu`/`bsearch` | `cpu-method=callfunc` | throughput, branch misses |
| Cache-heavy | `cache` | default | cache miss rates |
| SIMD/Vector | `matrix`/`vecmath` | `matrix-size=256` | bogo-ops/sec, FLOPS |
| Compression | `zlib` | default | throughput |
| Sorting/search | `qsort`/`bsearch` | default | ops/sec, branch prediction |

---

## Output Files

Results are collected in:
- `packages/cdn_bench/micro_cpu/cpu_run.log` - stress-ng output + YAML metrics

With perf hooks enabled:
- `perfstat` with CPU events (instructions, cycles, cache, branches)
- `mpstat` CPU utilization metrics (1s interval)

---

## Troubleshooting

### Permission Denied
```bash
chmod +x ./packages/cdn_bench/micro_cpu/run.sh
```

### stress-ng Not Found
Install manually:
```bash
# RHEL/CentOS
sudo dnf install stress-ng

# Ubuntu/Debian
sudo apt-get install stress-ng
```

### Cannot Set CPU Governor
Requires root privileges:
```bash
sudo cpupower frequency-set -g performance
```

Check current governor:
```bash
cpupower frequency-info | grep governor
```

---

## Baseline Setup (Before Benchmarking)

For accurate, comparable results across machines:

```bash
# 1. Pin CPU frequency to max
sudo cpupower frequency-set -g performance

# 2. Check system topology
lscpu

# 3. Verify NUMA configuration
numactl --hardware

# 4. Check stress-ng available stressors
stress-ng --stressors
```

---

## See Also

- [stress-ng Project](https://github.com/ColinIanKing/stress-ng)
- [stress-ng Man Page](https://manpages.ubuntu.com/manpages/jammy/man1/stress-ng.1.html)
