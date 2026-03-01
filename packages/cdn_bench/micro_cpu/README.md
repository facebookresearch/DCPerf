# CPU Micro Benchmark (micro_cpu)

CPU and memory microbenchmarking using sysbench. This benchmark evaluates CPU and memory subsystem performance for various Edge workload profiles including CDN edge hosts, caching machines, object storage, and high networking workloads.

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

Verify sysbench is available:
```bash
sysbench --version
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
- `sysbench` - Multi-threaded benchmark tool
- `cpupower` - CPU frequency scaling control (kernel-tools)
- `numactl` - NUMA topology utilities

---

## Running the Benchmark

### Basic Usage

```bash
# Run with default settings (cpu test, 10000 primes, all threads)
sudo ./benchpress_cli.py -b ehw run micro_cpu
```

### Custom Parameters

Override default parameters using the `-o` flag with the format `"micro_cpu:<args>"`:

```bash
# CDN Edge workload (small primes, latency-sensitive)
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=cdn_edge --time=60" run

# Memory test with random access
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=memory --memory-access-mode=rnd --memory-block-size=64" run

# Object storage profile (large primes)
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=object_storage --threads=32" run

# Full example with multiple parameters
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=cpu --cpu-max-prime=20000 --threads=16 --time=120 --governor=performance" run
```

---

## Cleanup

Reset CPU governor to default:

```bash
# Via Benchpress
sudo ./benchpress_cli.py -b ehw clean micro_cpu

# Or manually
sudo ./packages/cdn_bench/micro_cpu/cleanup_cpu_micro.sh
```

---

## Parameter Reference

### Test Configuration

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `test_type` | Workload profile to run | `cpu` | `cpu`, `memory`, `cdn_edge`, `read_optimized`, `caching`, `object_storage`, `networking` |
| `threads` | Number of worker threads | `0` (auto = nproc) | `1`, `8`, `16`, `32` |
| `time` | Test duration (seconds) | `60` | `30`, `60`, `120`, `300` |
| `governor` | CPU frequency governor | `performance` | `performance`, `ondemand`, `powersave` |

### CPU Test Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `cpu_max_prime` | Upper limit for prime number generation | `10000` | `3000`, `5000`, `10000`, `20000` |

### Memory Test Parameters

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `memory_block_size` | Memory block size per operation | `4K` | `64`, `4K`, `1M` |
| `memory_total_size` | Total memory to transfer | `100G` | `10G`, `50G`, `100G`, `200G` |
| `memory_oper` | Memory operation type | `read` | `read`, `write` |
| `memory_access_mode` | Memory access pattern | `seq` | `seq`, `rnd` |

---

## Common Workloads

### 1. CDN Edge Host

Simulates edge request processing: many small requests, branchy code, high syscall rate, latency-sensitive.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=cdn_edge --time=60" run
```

**What to look at:**
- `events/sec` → throughput capacity
- `avg / p95 latency` → tail latency sensitivity
- Scaling when threads > physical cores

> 💡 Uses small primes (5000) to approximate request-heavy edge logic.

### 2. Read-Optimized Drives

For search indexes, analytics reads — CPU doing checksums, decompression, parsing.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=read_optimized --memory-access-mode=seq" run
```

**With random access:**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=read_optimized --memory-access-mode=rnd" run
```

**What to look at:**
- `MiB/sec` → raw bandwidth
- Cross-socket scaling → NUMA effects
- Consistency under load

### 3. Large Caching Machines

For Redis/Memcached-style behavior: hot data in memory, pointer chasing, small ops.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=caching" run
```

**Single-thread latency probe:**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=caching --threads=1" run
```

**What to look at:**
- Single-thread bandwidth → core quality
- Multi-thread scaling → cache coherence & memory subsystem
- Variance between runs

### 4. Object Storage / Large File Server

For large buffers, checksums, encryption, compression — throughput over latency.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=object_storage --time=120" run
```

**Memory streaming variant:**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=memory --memory-block-size=1M --memory-total-size=200G" run
```

**What to look at:**
- Sustained throughput
- Power/thermal throttling over time
- SMT benefit (hyperthreading on vs off)

### 5. High Networking Workloads

For load balancers, proxies, L4/L7 boxes — packet processing, high syscall rate.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=networking --time=60" run
```

**Test under-subscribed cores (scaling behavior):**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=networking --threads=8" run
```

**What to look at:**
- Diminishing returns at high thread counts
- Latency spikes
- SMT behavior

### 6. Quick System Validation

Fast sanity check before deeper testing:

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_cpu:--test-type=cpu --cpu-max-prime=5000 --time=30" run
```

---

## Workload Selection Guide

| System Type | Test Type | Key Parameters | Primary Metric |
|-------------|-----------|----------------|----------------|
| CDN Edge | `cdn_edge` | Small primes (5000), high threads | events/sec, p95 latency |
| Read-optimized SSD | `read_optimized` | 4K blocks, sequential | MiB/sec |
| Cache nodes | `caching` | 64B blocks, random | bandwidth, variance |
| Object storage | `object_storage` | Large primes (20000) | sustained throughput |
| Networking | `networking` | Small primes (3000) | scaling, latency |

---

## Output Files

Results are collected in:
- `packages/cdn_bench/micro_cpu/cpu_run.log` — sysbench output

With perf hooks enabled:
- `perfstat` with CPU events (instructions, cycles, cache, branches)
- `mpstat` CPU utilization metrics (1s interval)

---

## Troubleshooting

### Permission Denied
```bash
chmod +x ./packages/cdn_bench/micro_cpu/run.sh
```

### sysbench Not Found
Install manually:
```bash
# RHEL/CentOS
sudo dnf install sysbench

# Ubuntu/Debian
sudo apt-get install sysbench
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
```

---

## See Also

- [Sysbench GitHub](https://github.com/akopytov/sysbench)
- [Sysbench Documentation](https://github.com/akopytov/sysbench#readme)
