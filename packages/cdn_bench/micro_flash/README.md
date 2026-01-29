# Flash Micro Benchmark (micro_flash)

Flash/Storage sustained read/write microbenchmark using FIO. This benchmark evaluates NVMe and SSD performance for various workload patterns including CDN machines, read-optimized storage, and large caching machines.

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
- Root or sudo access
- Target storage device (NVMe/SSD) or test directory

Verify FIO is available:
```bash
fio --version
```

---

## Installation

The install script automatically detects your Linux distribution and installs FIO:

```bash
# Via Benchpress
sudo ./benchpress_cli.py -b ehw install micro_flash

# Or manually
sudo ./packages/cdn_bench/micro_flash/install_flash_micro.sh
```

**What gets installed:**
- `fio` - Flexible I/O tester

---

## Running the Benchmark

### Basic Usage

```bash
# Run with default settings (randread, 4k blocks, 8 jobs)
sudo ./benchpress_cli.py -b ehw run micro_flash
```

### Custom Parameters

Override default parameters using the `-o` flag with the format `"micro_flash:<args>"`:

```bash
# CDN-style workload
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randrw --rwmixread=95 --bs=16k --numjobs=16 --iodepth=64" run

# Test a specific device with larger size
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--filename=/dev/nvme1n1 --size=100G" run

# Use io_uring engine with high priority
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--ioengine=io_uring --hipri=1" run

# Full example with multiple parameters
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randrw --rwmixread=60 --bs=32k --numjobs=12 --iodepth=32 --size=1T --runtime=300 --ramp_time=30" run
```

---

## Cleanup

Remove test files and artifacts:

```bash
# Via Benchpress
sudo ./benchpress_cli.py -b ehw clean micro_flash

# Or manually
sudo ./packages/cdn_bench/micro_flash/cleanup_flash_micro.sh
```

---

## Parameter Reference

### Target / Data Layout

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `filename` | File or block device to test | `/dev/nvme0n1` | `/dev/nvme0n1`, `/mnt/test/testfile` |
| `directory` | Directory for test files | `/mnt/test` | `/data/cache` |
| `size` | Total data size per job | `10G` | `10G`, `100G`, `1T` |
| `filesize` | Size of each file | `1G` | `256M`, `1G`, `10G` |
| `nrfiles` | Number of files per job | `4` | `1`, `4`, `16` |
| `offset` | Start offset in file/device | `0` | `0`, `1G`, `10G` |
| `offset_increment` | Offset increment per job | `1G` | `1G`, `10G` |
| `direct` | Use direct I/O (bypass page cache) | `1` | `0`, `1` |

### I/O Pattern

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `rw` | I/O access pattern | `randread` | `read`, `write`, `randread`, `randwrite`, `rw`, `randrw` |
| `rwmixread` | Percentage of reads in mixed workload | `70` | `50`, `70`, `95` |
| `bs` | Block size | `4k` | `4k`, `8k`, `16k`, `128k`, `1M` |
| `bsrange` | Range of block sizes | `4k-64k` | `4k-16k`, `4k-128k` |
| `random_distribution` | Random distribution model | `random` | `random`, `zipf`, `pareto`, `normal` |

### Concurrency

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `numjobs` | Number of parallel jobs | `8` | `1`, `4`, `8`, `16`, `32` |
| `iodepth` | Queue depth per job | `32` | `1`, `16`, `32`, `64`, `128` |
| `thread` | Use threads instead of processes | `1` | `0`, `1` |
| `group_reporting` | Aggregate results from all jobs | `1` | `0`, `1` |

### Timing

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `runtime` | Runtime duration (seconds) | `300` | `60`, `300`, `600`, `3600` |
| `time_based` | Run workload based on time | `1` | `0`, `1` |
| `ramp_time` | Warm-up time before measurement (seconds) | `30` | `0`, `30`, `60` |
| `timeout` | Abort job after timeout (seconds) | `600` | `300`, `600`, `1200` |
| `loops` | Number of job repetitions | `1` | `1`, `3`, `5` |

### I/O Engine

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `ioengine` | I/O backend engine | `libaio` | `libaio`, `io_uring`, `sync`, `psync`, `mmap` |
| `sync` | Force synchronous I/O | `0` | `0`, `1` |
| `hipri` | Use high-priority polling (io_uring) | `0` | `0`, `1` |

### Rate Limiting

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `rate` | Bandwidth limit | *(none)* | `500M`, `1G` |
| `rate_iops` | IOPS limit | *(none)* | `10000`, `50000` |
| `rate_process` | Apply rate per job instead of total | `0` | `0`, `1` |

### CPU / NUMA Affinity

| Parameter | Description | Default | Example Values |
|-----------|-------------|---------|----------------|
| `cpus_allowed` | CPU affinity mask | *(none)* | `0-7`, `0,2,4,6` |
| `cpus_allowed_policy` | CPU sharing policy | `shared` | `shared`, `split` |
| `numa_cpu_nodes` | Bind jobs to NUMA CPU nodes | *(none)* | `0`, `0,1` |
| `numa_mem_policy` | NUMA memory allocation policy | `default` | `default`, `local`, `bind` |

---

## Common Workloads

### 1. CDN Edge Host Evaluation

Simulates edge cache behavior: read-heavy, small-medium objects, many concurrent clients.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randrw --rwmixread=95 --bs=16k --numjobs=16 --iodepth=64 --runtime=300 --ramp_time=30" run
```

**What to look at:**
- p99 read latency (more important than average)
- IOPS stability over time
- Latency blowups under queue depth

### 2. Read-Optimized Drives

For search indexes, analytics replicas, AI embeddings — fast reads, infrequent writes.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randread --bs=4k --numjobs=8 --iodepth=128 --ioengine=io_uring --runtime=240 --ramp_time=20" run
```

**What to look at:**
- Read latency percentiles under pressure
- Read/write interference (if adding background writes)
- Tail latency regression when writes appear

**Optional: Add light background writes**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randwrite --bs=16k --numjobs=1 --iodepth=4 --rate_iops=500" run
```

### 3. Large Caching Machines

For Redis/Memcached backing store, NVMe cache — write bursts, eviction, mixed access.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randrw --rwmixread=60 --bs=32k --numjobs=12 --iodepth=32 --size=1T --runtime=300 --ramp_time=30" run
```

**What to look at:**
- Write latency spikes
- Bandwidth consistency
- Performance after sustained writes (GC behavior)

**Optional: Cache warm-up phase first**
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=write --bs=128k --numjobs=4 --iodepth=16 --runtime=120" run
```

### 4. Object Storage / Large File Servers

For S3-like workloads, media storage, backups — focus on MB/s, not IOPS.

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=read --bs=1M --numjobs=4 --iodepth=16 --runtime=180" run
```

### 5. Quick System Validation

Fast sanity check before deeper testing:

```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--rw=randread --bs=4k --numjobs=4 --iodepth=32 --runtime=60 --ramp_time=10" run
```

---

## Workload Selection Guide(Not fixed just good examples)

| System Type | FIO Pattern | Key Parameters |
|-------------|-------------|----------------|
| CDN edge | `randrw` 95% read | `bs=16k`, high concurrency |
| Read-optimized SSD | `randread` | `bs=4k`, deep queues (`iodepth=128`) |
| Cache nodes | `randrw` 60% read | `bs=32k`, sustained writes |
| Object storage | seq `read`/`write` | `bs=1M`, large blocks |
| Database (OLTP) | `randrw` 70% read | `bs=8k`, `numjobs=8`, `iodepth=32` |

---

## Interpreting Results

### Key Metrics

| Metric | Description | What to Watch |
|--------|-------------|---------------|
| **IOPS** | I/O operations per second | Higher is better for random workloads |
| **BW** | Bandwidth (MB/s) | Higher is better for sequential workloads |
| **lat (avg)** | Average latency | Lower is better |
| **clat percentiles** | Completion latency distribution | p99/p99.9 more important than average |
| **slat** | Submission latency | Should be very low (<10µs) |

### Latency Percentiles

Focus on tail latencies, not averages:

- **p50 (median)**: Typical request latency
- **p95**: 95% of requests complete within this time
- **p99**: Critical for SLA compliance
- **p99.9**: Tail latency — reveals worst-case behavior

### Common Anti-Patterns to Avoid 🚨

| Anti-Pattern | Problem |
|--------------|---------|
| `--bs=4k --rw=randread` alone | Unrealistic isolation |
| No `ramp_time` | Inflated early results |
| No latency percentiles | Meaningless averages |
| `direct=0` (page cache enabled) | Fantasy numbers |
| Very short runtime | Not reaching steady state |

---

## Output Files

Results are collected in:
- `packages/cdn_bench/micro_flash/fio_run.log` — FIO output

With perf hooks enabled:
- `iostat` metrics (1s interval)
- `perfstat` with cache events
- `mpstat` CPU metrics

---

## Troubleshooting

### Permission Denied
```bash
chmod +x ./packages/cdn_bench/micro_flash/run.sh
```

### Device Busy
Ensure no filesystems are mounted on the test device:
```bash
umount /dev/nvme0n1p1  # if applicable
```

### io_uring Not Available
Fall back to libaio:
```bash
sudo ./benchpress_cli.py -b ehw -o "micro_flash:--ioengine=libaio" run
```

Check kernel support:
```bash
cat /proc/kallsyms | grep io_uring
```

---

## See Also

- [FIO Documentation](https://fio.readthedocs.io/)
- [FIO Man Page](https://linux.die.net/man/1/fio)
