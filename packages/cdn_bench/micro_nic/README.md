# NIC Micro Benchmark (micro_nic)

NIC microbenchmarking using iperf3. This benchmark evaluates network interface card performance with configurability for high parallelization and NUMA-aware tests. The micro requires two peerable hosts: one acting as the client (traffic generator) and one as the server (machine under test).

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Running the Benchmark](#running-the-benchmark)
- [Cleanup](#cleanup)
- [Parameter Reference](#parameter-reference)
- [Common Workloads](#common-workloads)
- [Output and Metrics](#output-and-metrics)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

### Installation

Run the install command on **both** server and client machines:

```bash
sudo ./benchpress_cli.py -b ehw install micro_nic
```

**What gets installed:**
- `iperf3` - Network performance measurement tool
- Executable permissions on `run.sh`

---

## Running the Benchmark

### Two-Machine Setup (Recommended)

**Machine 1 - Server :**
```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"<AVAILABLE_PORT>","interval":"<INTERVAL(s)>","extra_args":"<ARGS>"}'
```

**Machine 2 - Client (Traffic Generator):**
```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"<AVAILABLE_PORT>","duration":"<TEST_DURATION(s)>","parallel":"<NUM_PARALLEL_IPERF_STREAMS>","interval":"<INTERVAL(s)>","extra_args":"<ARGS>"}'
```

---

## Cleanup

Remove artifacts and stop any lingering iperf3 processes:

```bash
sudo ./benchpress_cli.py -b ehw clean micro_nic
```

---

## Parameter Reference

### Server Role Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `port` | Port(s) to listen on (comma-separated for multiple) | `5201` |
| `interval` | Reporting interval in seconds | `1` |
| `extra_args` | Additional iperf3 flags (e.g., `-N 0 -M 0` for NUMA binding) | (empty) |

### Client Role Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `server_ip` | Server IP address(es) (comma-separated for multiple) | (required) |
| `port` | Server port(s) (comma-separated for multiple) | `5201` |
| `duration` | Test duration in seconds | `60` |
| `parallel` | Number of parallel streams per instance | `1` |
| `interval` | Reporting interval in seconds | `1` |
| `extra_args` | Additional iperf3 flags | (empty) |

### Extra Args Reference

The `extra_args` parameter accepts any valid iperf3 flags. Common options:

| Flag | Description |
|------|-------------|
| `-N <node>` | NUMA CPU node binding (numactl --cpunodebind) |
| `-M <node>` | NUMA memory node binding (numactl --membind) |
| `-V` | Verbose output |
| `-f g` | Output format in Gbits/sec |
| `-f m` | Output format in Mbits/sec |
| `-R` | Reverse mode (server sends, client receives) |
| `-u` | Use UDP instead of TCP |
| `-b <bandwidth>` | Target bandwidth (e.g., `10G`, `1M`) |
| `-l <length>` | Buffer length |
| `-w <window>` | TCP window size |
| `-Z` | Zero-copy mode |
| `-A <affinity>` | CPU affinity (e.g., `0,1` or `0-3`) |

---

## Common Workloads

### 1. CDN Edge Host Evaluation

Evaluate NIC performance for CDN edge host qualification with NUMA-aware core pinning and progressive stream scaling. This methodology tests single-process receive efficiency, which is more representative of CDN edge proxy behavior than multi-process approaches.

#### Progressive Stream Testing

Run a series of tests with decreasing stream counts to characterize single-flow ceilings and multi-flow scaling:

**8-Stream Test (Maximum Parallelism):**
```bash
# Server (DUT, receiver):
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-A 0"}'

# Client (traffic generator):
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"8","interval":"10","extra_args":"-f g -A 0"}'
```

**4-Stream Test (Moderate Parallelism):**
```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-A 0"}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"4","interval":"10","extra_args":"-f g -A 0"}'
```

**2-Stream Test (Single-Flow Characterization):**
```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-A 0"}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"2","interval":"10","extra_args":"-f g -A 0"}'
```

**What to look at:**
- iperf3 aggregate throughput (Gbps) — should approach 100G with 8 streams
- Per-stream fairness — balanced distribution indicates good RSS hashing
- Retransmit counts — high retransmits suggest buffer or flow control issues
- Server CPU utilization — should remain low (<5%) for efficient NIC/driver
- RX dropped packets — should be zero for lossless operation

**Expected results (100G NIC):**
- 8-stream: ~92-93 Gbps aggregate (near line rate)
- 4-stream: ~88-92 Gbps aggregate (slight reduction from 8-stream)
- 2-stream: ~50 Gbps aggregate (~25 Gbps per flow TCP ceiling)

### 2. CDN Edge Host with NUMA Awareness and Multi-Port

For multi-socket systems or hosts with multiple NICs, ensure cores are pinned to the same NUMA node as the NIC. This eliminates cross-NUMA traffic and maximizes throughput.

**Prerequisites:**
```bash
# 1. Check NIC's NUMA node
cat /sys/class/net/eth0/device/numa_node

# 2. Check NUMA topology
numactl --hardware

# 3. Identify cores on the NIC's NUMA node
lscpu | grep "NUMA node0"  # or node1 depending on NIC location
```

**8-Port Multi-Instance Configuration:**

```bash
# Server (Machine Under Test) - 8 instances pinned to NUMA node 0:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201,5202,5203,5204,5205,5206,5207,5208","interval":"10","extra_args":"-A 0,10,20,30,40,50,60,70 -N 0 -M 0"}'
```

```bash
# Client (Traffic Generator) - 8 instances with matching affinity:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>,<SERVER_IP>,<SERVER_IP>,<SERVER_IP>,<SERVER_IP>,<SERVER_IP>,<SERVER_IP>,<SERVER_IP>","port":"5201,5202,5203,5204,5205,5206,5207,5208","duration":"60","parallel":"1","interval":"10","extra_args":"-f g -A 0,10,20,30,40,50,60,70 -N 0 -M 0"}'
```

**Key points:**
- `-A 0,10,20,30,40,50,60,70` assigns each iperf3 instance to a dedicated core
- `-N 0 -M 0` binds both CPU and memory to NUMA node 0 (where the NIC resides)
- Spread cores across physical cores (e.g., 0, 10, 20, 30...) to avoid SMT sibling contention
- The number of CPU affinities should match the number of ports/instances

**What to look at:**
- Aggregate throughput across all instances
- Per-instance fairness
- NUMA node utilization (should be isolated to one node)
- Cross-NUMA traffic (should be minimal)

### 3. High-Performance Single-Port (NIC Saturation)

For quick NIC qualification without multi-port complexity:

```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-N <NUMA_NODE> -M <NUMA_NODE>"}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"32","interval":"10","extra_args":"-f g -N <NUMA_NODE> -M <NUMA_NODE> -V"}'
```

**What to look at:**
- Achieved throughput vs NIC line rate
- CPU overhead (sys, softirq)
- Retransmits and drops

### 4. UDP Testing with Bandwidth Limit

For UDP-based CDN workloads or packet-per-second characterization:

```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-N <NUMA_NODE> -M <NUMA_NODE>"}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"8","interval":"10","extra_args":"-u -b 10G -N <NUMA_NODE> -M <NUMA_NODE>"}'
```

### 5. Reverse Mode (Download Test)

Test server-to-client direction (simulating origin fetch or cache fill):

```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"10","extra_args":"-N <NUMA_NODE> -M <NUMA_NODE>"}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"60","parallel":"16","interval":"10","extra_args":"-R -f g -N <NUMA_NODE> -M <NUMA_NODE> -V"}'
```

### 6. Quick System Validation

Fast sanity check before deeper testing:

```bash
# Server:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"5201","interval":"1","extra_args":""}'

# Client:
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"5201","duration":"30","parallel":"4","interval":"1","extra_args":"-f g"}'
```

---

## Workload Selection Guide

| System Type | Configuration | Key Parameters | Primary Metric |
|-------------|---------------|----------------|----------------|
| CDN Edge Qualification | Progressive 8/4/2-stream | `parallel=8,4,2`, single process | Throughput scaling, retransmits |
| CDN Edge Multi-NUMA Multi-Core | 8-port multi-instance | `-A <cores> -N <node> -M <node>` | NUMA-local throughput, fairness |
| UDP/PPS Testing | UDP mode | `-u -b <rate>` | Packet rate, loss rate |
| Download/Origin | Reverse mode | `-R` | Server egress throughput |
| Quick Validation | Basic 4-stream | `parallel=4`, short duration | Basic connectivity, sanity check |

---

## Output and Metrics

### Results Directory

After a run, results are stored in `benchmark_metrics_<uuid>/`:

```
benchmark_metrics_<uuid>/
├── micro_nic_metrics_<timestamp>_iter_None.json    # Parsed metrics
├── micro_nic_system_specs_<timestamp>.json         # System specifications
├── nic_run.log                                      # Full iperf3 output log
├── net-stat.csv                                     # Network statistics
├── net-stat.log
├── perf-stat.csv                                    # Performance counters
├── perf-stat.log
├── vmstat.csv                                       # Virtual memory stats
├── vmstat.log
├── mpstat.csv                                       # CPU statistics
├── mpstat.log
└── mem-stat.csv                                     # Memory statistics
```

### Log File

The `nic_run.log` file contains:
- NIC information (MAC, MTU, speed, driver, ring buffers, queues)
- Network kernel parameters (TCP buffers, congestion control)
- CPU and NUMA topology
- Full iperf3 benchmark output
- Post-benchmark NIC statistics (RX/TX bytes, packets, errors, drops)

### Key Metrics

**Throughput Metrics:**
- `throughput_gbps` — Aggregate throughput in Gbps
- `throughput_per_stream_gbps` — Per-stream throughput for fairness analysis

**Reliability Metrics:**
- `retransmits` — TCP retransmit count (high values indicate congestion or buffer issues)
- `rx_dropped` — RX packets dropped (should be zero for lossless operation)
- `rx_errors` — RX errors (should be zero)

**System Metrics:**
- Server/client CPU utilization (usr, sys, softirq, irq)
- Memory usage
- Network interface counters

---

## Troubleshooting

### "the job micro_nic does not have roles"

Ensure you're using `--role server` or `--role client` flag:

```bash
# Wrong:
sudo ./benchpress_cli.py -b ehw run micro_nic -i 1 ...

# Correct:
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 ...
```

### Server Hangs After Client Completes

The server runs in one-off mode (`-1`) by default and should exit after the client disconnects. If it hangs:
- Check that the client completed successfully
- Ensure you run the cleanup script to kill any hanging iperf3 processes

### Connection Refused

1. Verify the server is running and listening on the correct port
2. Check firewall rules: `firewall-cmd --list-ports`
3. Ping the client to server and viceversa: `ping <server_ip>`

### Poor Performance

1. **Use CPU pinning**:
2. **Check NUMA alignment**: Ensure `-N` and `-M` match the NIC's NUMA node locality
3. **Increase parallel streams**: Use `-P 32` for NICs which scale at higher parallelization
4. **Increase port count**: Comma-separated `server_ip` to reach NIC saturation limits
5. **Check for CPU throttling**: `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
6. **Verify MTU settings**: `ip link show eth0`

---
### CPU Pinning Matters

Single-core context switching overhead is the primary bottleneck when running multiple iperf3 instances without explicit CPU affinity. By pinning each iperf3 instance to a dedicated CPU core, we eliminate this overhead and achieve near line-rate throughput.

### Per-Instance CPU Affinity

The `-A` flag in `extra_args` supports **comma-separated CPU cores** that are automatically distributed to each iperf3 instance. This works for **both server and client modes**. For example, `-A 0,10,20,30,40,50,60,70` assigns:
- Instance 1 (port 5201) → CPU 0
- Instance 2 (port 5202) → CPU 10
- Instance 3 (port 5203) → CPU 20
- Instance 4 (port 5204) → CPU 30
- Instance 5 (port 5205) → CPU 40
- Instance 6 (port 5206) → CPU 50
- Instance 7 (port 5207) → CPU 60
- Instance 8 (port 5208) → CPU 70

The number of CPU affinities provided should match the number of ports/instances. If fewer affinities are provided than instances, only the first N instances will have CPU pinning applied.

### Choosing CPU Cores

1. **Check NIC's NUMA node**: `cat /sys/class/net/eth0/device/numa_node`
2. **Check NUMA topology**: `numactl --hardware`
3. **Spread cores**: Use cores spread across the physical cores (e.g., 0, 10, 20, 30...) to avoid SMT sibling contention
4. **Match NIC's NUMA node**: For multi-socket systems, use cores on the same NUMA node as the NIC

### iperf3 Not Found

Run the install command:

```bash
sudo ./benchpress_cli.py -b ehw install micro_nic
```

---

## Baseline Setup (Before Benchmarking)

For accurate, comparable results across machines:

```bash
# 1. Check NIC information
ethtool -i eth0 # or other iface

# 2. Check NIC NUMA node
cat /sys/class/net/eth0/device/numa_node

# 3. Verify NUMA configuration
numactl --hardware

# 4. Check available CPU cores
lscpu

# 5. Verify iperf3 version
iperf3 --version
```
