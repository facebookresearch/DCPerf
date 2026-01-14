# NIC Micro Benchmark Runbook

## Overview

The NIC Micro benchmark uses iperf3 to stress test network interface cards with the configurability needed for high parallelization and NUMA-aware tests. The micro requires two peerable hosts, one acting as the client, and one as the server(Our machine under test).

## Prerequisites

### Installation

Run the install command on both server and client machines:

```bash
sudo ./benchpress_cli.py -b ehw install micro_nic
```

This installs:
- iperf3 package
- Makes run.sh executable

## Quick Start

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

## Detailed Usage

### Server Role

The server listens for incoming iperf3 connections. It runs in **one-off mode** by default, meaning it will exit after the client test completes (required for DC Perf metrics collection).

#### Basic Server Command

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"<PORT>","interval":"<INTERVAL>"}'
```

#### Server Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `port` | Port(s) to listen on (comma-separated for multiple) | `5201` |
| `interval` | Reporting interval in seconds | `1` |
| `extra_args` | Additional iperf3 flags (e.g., `-N 0 -M 0` for NUMA binding) | (empty) |

#### Multi-Port Server (Multiple Instances)

To run multiple iperf3 server instances on different ports:

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role server -i 1 --role_input='{"port":"<PORT_1>,<PORT_2>","interval":"<INTERVAL>","extra_args":"-N <NUMA_NODE> -M <NUMA_NODE>"}'
```

This spawns two iperf3 servers listening on the specified ports.

---

### Client Role

The client generates traffic to stress the server's NIC.

#### Basic Client Command

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"<PORT>","duration":"<DURATION>","parallel":"<PARALLEL_STREAMS>","interval":"<INTERVAL>"}'
```

#### Client Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `server_ip` | Server IP address(es) (comma-separated for multiple) | (required) |
| `port` | Server port(s) (comma-separated for multiple) | `5201` |
| `duration` | Test duration in seconds | `60` |
| `parallel` | Number of parallel streams per instance | `1` |
| `interval` | Reporting interval in seconds | `1` |
| `extra_args` | Additional iperf3 flags | (empty) |

#### High-Performance Client (NIC Saturation)

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"<PORT>","duration":"<DURATION>","parallel":"32","interval":"<INTERVAL>","extra_args":"-f g -N <NUMA_NODE> -M <NUMA_NODE> -V"}'
```

#### Multi-Server Client (Parallel Connections)

To connect to multiple server instances simultaneously:

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>,<SERVER_IP>","port":"<PORT_1>,<PORT_2>","duration":"<DURATION>","parallel":"32","interval":"<INTERVAL>","extra_args":"-f g -N <NUMA_NODE> -M <NUMA_NODE> -V"}'
```

---

## Extra Args Reference

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

### Example: UDP Test with Bandwidth Limit

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"<PORT>","duration":"<DURATION>","parallel":"8","interval":"<INTERVAL>","extra_args":"-u -b <BANDWIDTH> -N <NUMA_NODE> -M <NUMA_NODE>"}'
```

### Example: Reverse Mode (Download Test)

```bash
sudo ./benchpress_cli.py -b ehw run micro_nic --role client -i 1 --role_input='{"server_ip":"<SERVER_IP>","port":"<PORT>","duration":"<DURATION>","parallel":"16","interval":"<INTERVAL>","extra_args":"-R -N <NUMA_NODE> -M <NUMA_NODE> -V"}'
```

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

1. **Check NUMA alignment**: Ensure `-N` and `-M` match the NIC's NUMA node locality
2. **Increase parallel streams**: Use `-P 32` for NICs which scale at higher parallelization
3. **Increase port count**: Coma separated `server_ip` to reach NIC saturation limits
4. **Check for CPU throttling**: `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
5. **Verify MTU settings**: `ip link show eth0`

### iperf3 Not Found

Run the install command:

```bash
sudo ./benchpress_cli.py -b ehw install micro_nic
```

---

## Cleanup

To clean up after testing:

```bash
sudo ./benchpress_cli.py -b ehw clean micro_nic
```

This removes any temporary files and stops any lingering iperf3 processes.
