# Memory Micro Benchmark Runbook

## Overview

**STREAM** is a widely used tool for measuring sustainable memory bandwidth. This benchmark uses OpenMP parallelization to saturate all memory channels and provides accurate full-system memory bandwidth measurements.

The benchmark automatically:
- Compiles STREAM with `-O3 -fopenmp` for optimal parallelization
- Uses all available CPU threads via `OMP_NUM_THREADS`
- Applies NUMA interleaving via `numactl --interleave=all` for optimal memory access

---

## Quick Start

### 1. Install the Memory Microbenchmark

```bash
sudo ./benchpress_cli.py -b ehw install micro_mem
```

This installs:
- GCC compiler
- numactl (for NUMA optimization)
- Required perf tools

---

### 2. Run the Memory Microbenchmark

**Recommended command for 100G NIC servers (large L3 cache):**

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_mem:500000000 100" run micro_mem
```

**Parameters:**
- `500000000` - Array size (500M elements = ~11 GiB working set)
- `100` - Number of iterations

**For maximum accuracy (production benchmarking):**

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_mem:1000000000 100" run micro_mem
```

- `1000000000` - 1B elements = ~22 GiB working set (recommended for 336 MiB L3 cache systems)

---

## Parameter Selection Guide

### Array Size (STREAM_ARRAY_SIZE)

The array size **must be much larger than L3 cache** to ensure cache misses and accurate memory bandwidth measurement.

| System L3 Cache | Minimum Array Size | Recommended | Working Set |
|-----------------|-------------------|-------------|-------------|
| 96 MiB | 100M | **500M** | ~11 GiB |
| 336 MiB | 200M | **1B** | ~22 GiB |

**Formula:** Working Set = ARRAY_SIZE × 8 bytes × 3 arrays

**⚠️ Avoid small array sizes:**

```bash
# BAD: 20M elements = ~457 MiB, barely exceeds L3 cache
sudo ./benchpress_cli.py -b ehw -o "micro_mem:20000000 5" run micro_mem

# GOOD: 500M elements = ~11 GiB, properly stresses memory
sudo ./benchpress_cli.py -b ehw -o "micro_mem:500000000 100" run micro_mem
```

### NTIMES (Iterations)

- **Minimum:** 10 iterations
- **Recommended:** 100 iterations for stable results
- Higher values reduce variance in reported bandwidth

---

## OpenMP Parallelization

The benchmark automatically configures OpenMP for optimal performance:

| Setting | Value | Purpose |
|---------|-------|---------|
| `OMP_NUM_THREADS` | $(nproc) | Uses all available CPUs |
| `OMP_PROC_BIND` | spread | Distributes threads across cores |
| `OMP_PLACES` | threads | Binds to hardware threads |

### NUMA Optimization

The benchmark uses `numactl --interleave=all` to:
- Distribute memory allocations across all NUMA nodes
- Maximize aggregate memory bandwidth
- Avoid NUMA locality bottlenecks

---

## Expected Results

### Properly Parallelized STREAM (with this fix)

| System | Memory Config | Expected Triad | Efficiency |
|--------|---------------|----------------|------------|
| 64-core, 8-channel DDR5-5600 | 358 GB/s theoretical | **~250-280 GB/s** | 70-78% |
| 64-core, 8-channel DDR5-6400 | 410 GB/s theoretical | **~290-320 GB/s** | 70-78% |

### Single-Threaded STREAM (old behavior)

| System | Expected Triad | Issue |
|--------|----------------|-------|
| Any | ~15 GB/s | Only uses 1 memory channel |

If you see ~15 GB/s, the benchmark is not properly parallelized.

---

## Output and Metrics

### Results Directory

After a run, results are stored in `benchmark_metrics_<uuid>/`:

```text
benchmark_metrics_<uuid>/
├── micro_mem_metrics_<timestamp>_iter_None.json    # Parsed metrics
├── micro_mem_system_specs_<timestamp>.json         # System specifications
├── stream_run.log                                   # Full STREAM output log
├── perf-stat.csv                                    # Performance counters
├── vmstat.csv                                       # Virtual memory stats
├── mpstat.csv                                       # CPU statistics
└── mem-stat.csv                                     # Memory statistics
```

### Key Metrics

| Metric | Description | Unit |
|--------|-------------|------|
| `copy_best_MBps` | Best Copy bandwidth | MB/s |
| `scale_best_MBps` | Best Scale bandwidth | MB/s |
| `add_best_MBps` | Best Add bandwidth | MB/s |
| `triad_best_MBps` | Best Triad bandwidth (most important) | MB/s |
| `triad_avg_MBps` | Average Triad bandwidth | MB/s |

### Why Triad is Most Important

| Operation | Formula | Memory Ops | Bytes/Element |
|-----------|---------|------------|---------------|
| Copy | c[i] = a[i] | 1 read, 1 write | 16 |
| Scale | b[i] = scalar × c[i] | 1 read, 1 write | 16 |
| Add | c[i] = a[i] + b[i] | 2 reads, 1 write | 24 |
| **Triad** | a[i] = b[i] + scalar × c[i] | 2 reads, 1 write | 24 |

Triad combines multiply-add with multiple memory streams, reflecting real workloads.

---

## Cleanup

```bash
sudo ./benchpress_cli.py -b ehw clean micro_mem
```

This removes binaries, logs, and temporary files.

---

## Troubleshooting

### Low Bandwidth (~15 GB/s instead of ~250+ GB/s)

**Cause:** OpenMP not working properly.

**Check:**
```bash
# Verify OpenMP threads
OMP_DISPLAY_ENV=true ./stream
```

**Solution:** Ensure `libomp` or `libgomp` is installed:
```bash
# CentOS/RHEL
dnf install -y libgomp

# Ubuntu
apt install -y libomp-dev
```

### "numactl not found" Warning

**Solution:**
```bash
# CentOS/RHEL
dnf install -y numactl

# Ubuntu
apt install -y numactl
```

### Out of Memory

**Cause:** Array size too large for available RAM.

**Solution:** Reduce array size. Working set should be < 50% of total RAM.

### Permission Denied

Run with `sudo` for perf counter access.

---

## Vendor Addendum

For detailed performance analysis, run with additional perf events:

```json
{
  "perf": {
    "perfstat": {
      "additional_events": [
        "cache-references",
        "cache-misses",
        "LLC-load-misses",
        "LLC-store-misses",
        "dTLB-load-misses",
        "dTLB-store-misses"
      ]
    },
    "vmstat": {
      "interval": 1
    },
    "mpstat": {
      "interval": 1
    }
  }
}
```

---

## References

- [STREAM Benchmark Homepage](https://www.cs.virginia.edu/stream/)
- [OpenMP Best Practices](https://www.openmp.org/resources/)
- [NUMA Memory Policies](https://man7.org/linux/man-pages/man8/numactl.8.html)
