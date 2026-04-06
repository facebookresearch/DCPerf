# CDN Benchmark (foss_revproxy)

A CDN reverse proxy benchmark built on [Proxygen](https://github.com/facebook/proxygen)'s
coro API. It orchestrates three components — `content_server`, `proxy_server`, and
`traffic_client` — to simulate realistic CDN traffic patterns for hardware evaluation.

## Quick Start

```bash
# Install (builds proxygen + foss_revproxy from source)
./benchpress_cli.py -b ehw install cdn_bench

# Run default profile (60s, 1k RPS, H2 cleartext)
./benchpress_cli.py -b ehw run cdn_bench
```

## Parameters

| Parameter | Flag | Default | Description |
|-----------|------|---------|-------------|
| `duration` | `-d` | 60 | Test duration in seconds |
| `target_rps` | `-r` | 1000 | Target requests per second |
| `num_connections` | `-c` | 4 | Number of concurrent connections |
| `streams_per_connection` | `-S` | 100 | Max concurrent streams per connection (H2 only) |
| `protocol` | `-p` | h2 | Protocol: `h1` or `h2` |
| `metrics_interval` | `-I` | 10 | Proxy metrics reporting interval in seconds |

Override parameters at runtime:

```bash
./benchpress_cli.py -b ehw run cdn_bench -i '{"duration": "120", "target_rps": "5000"}'
```

## Distributed Mode

For multi-host testing, each role runs on a separate host. The server and proxy
roles include a built-in grace period: they run for `duration + 10 seconds` before
shutting down automatically. This ensures they remain available for the full
duration of the client's traffic generation.

**Startup order matters — always start in this sequence:**

1. **Server** — start first, waits for `duration + 10s`
2. **Proxy** — start second, waits for `duration + 10s`
3. **Client** — start last, runs for exactly `duration`

```bash
# 1. Server host — start content server
./benchpress_cli.py -b ehw run cdn_bench -r server \
  -i '{"ports":"8082","protocol":"h2","duration":"60"}'

# 2. Proxy host — start proxy, pointing to server host
./benchpress_cli.py -b ehw run cdn_bench -r proxy \
  -i '{"backend_hosts":"<server_ip>","backend_ports":"8082","ports":"8081","protocol":"h2","duration":"60"}'

# 3. Client host — send traffic to proxy
./benchpress_cli.py -b ehw run cdn_bench -r client \
  -i '{"proxy_targets":"<proxy_ip>:8081","duration":"60","target_rps":"1000","num_connections":"4","streams_per_connection":"100"}'
```

## Job Profiles

### High Throughput

300s duration, 50k target RPS, 16 connections, 200 streams/connection. Designed
for maximum RPS saturation testing to stress CPU and NIC subsystems.

```bash
# 1. Server host
./benchpress_cli.py -b ehw run cdn_bench -r server \
  -i '{"ports":"8082","protocol":"h2","duration":"300"}'

# 2. Proxy host
./benchpress_cli.py -b ehw run cdn_bench -r proxy \
  -i '{"backend_hosts":"<server_ip>","backend_ports":"8082","ports":"8081","protocol":"h2","duration":"300"}'

# 3. Client host
./benchpress_cli.py -b ehw run cdn_bench -r client \
  -i '{"proxy_targets":"<proxy_ip>:8081","duration":"300","target_rps":"50000","num_connections":"16","streams_per_connection":"200"}'
```

### Sustained

600s duration, 10k target RPS, 8 connections. Designed for thermal and power
evaluation at steady-state moderate load.

```bash
# 1. Server host
./benchpress_cli.py -b ehw run cdn_bench -r server \
  -i '{"ports":"8082","protocol":"h2","duration":"600"}'

# 2. Proxy host
./benchpress_cli.py -b ehw run cdn_bench -r proxy \
  -i '{"backend_hosts":"<server_ip>","backend_ports":"8082","ports":"8081","protocol":"h2","duration":"600"}'

# 3. Client host
./benchpress_cli.py -b ehw run cdn_bench -r client \
  -i '{"proxy_targets":"<proxy_ip>:8081","duration":"600","target_rps":"10000","num_connections":"8"}'
```

## Multi-Instance Scaling

Each role supports multiple instances via comma-separated ports and targets.
This enables horizontal scaling for NIC saturation and backend fleet simulation.

```bash
# 1. Server host — 3 content servers on different ports
./benchpress_cli.py -b ehw run cdn_bench -r server \
  -i '{"ports":"8082,8083,8084","protocol":"h2","duration":"60"}'

# 2. Proxy host — 2 proxy instances, each routing to all 3 backends
./benchpress_cli.py -b ehw run cdn_bench -r proxy \
  -i '{"backend_hosts":"<server_ip>,<server_ip>,<server_ip>","backend_ports":"8082,8083,8084","ports":"8081,8091","protocol":"h2","duration":"60"}'

# 3. Client host — targeting both proxies
./benchpress_cli.py -b ehw run cdn_bench -r client \
  -i '{"proxy_targets":"<proxy_ip>:8081,<proxy_ip>:8091","duration":"60","target_rps":"50000"}'
```

## Output Metrics

Results are collected by the `cdn_bench` parser and include:

**Client metrics:** `client_requests_sent`, `client_responses_received`, `client_errors`,
`client_resets`, `client_elapsed_ms`, `client_actual_rps`

**Proxy metrics:** `proxy_requests_received`, `proxy_requests_succeeded`,
`proxy_requests_failed`, `proxy_success_rate_pct`, `proxy_actual_rps`,
`proxy_avg_latency_ms`, `proxy_avg_backend_latency_ms`, `proxy_retries_attempted`,
`proxy_retries_succeeded`

## System Requirements

- **OS:** CentOS 8+ or Ubuntu 20.04+
- **Compiler:** GCC 10+ or Clang 12+ (C++20 required)
- **Memory:** 8 GB minimum for building (16 GB recommended)
- **Disk:** ~5 GB for build artifacts

The install script automatically installs system dependencies via `dnf` or `apt`.

## Build Details

The install script:
1. Clones proxygen at a pinned version from GitHub
2. Builds the full dependency chain from source (boost, folly, fizz, wangle, mvfst, proxygen)
3. Builds foss_revproxy against the installed proxygen
4. Installs binaries to `benchmarks/cdn_bench/`

To force a rebuild:
```bash
./benchpress_cli.py -b ehw clean cdn_bench
./benchpress_cli.py -b ehw install cdn_bench
```

## Troubleshooting

**OOM during build:** On high-core-count servers, the build may use too much memory.
Limit parallelism:
```bash
taskset -c 0-15 ./benchpress_cli.py -b ehw install cdn_bench
```

**Port conflicts:** The benchmark uses ports 8081 (proxy) and 8082 (content server).
If these ports are in use, the health check will fail with a timeout error. Kill any
processes using those ports before running.

**Binary not found:** If you see `ERROR: traffic_client not found`, run the install step
first. The binaries must be present at `benchmarks/cdn_bench/`.
