# CDN Benchmark Architecture

## Overview

The CDN Benchmark simulates a simplified CDN reverse proxy topology using three
foss_revproxy binaries built on Facebook's Proxygen HTTP library.

## Components

### content_server
Origin backend that serves synthetic content. Listens on a configurable port
(default 8082) and responds to HTTP requests with generated payloads.

### proxy_server
Reverse proxy that sits between clients and content servers. Accepts client
requests on its listen port (default 8081) and forwards them to configured
backend content servers. Tracks per-request latency, success rates, and retry
statistics.

### traffic_client
Load generator that sends HTTP requests at a target RPS to the proxy server.
Supports configurable connection count, streams per connection (H2 multiplexing),
and duration. Reports final statistics including actual RPS, error counts, and
elapsed time.

## Single-Host Topology

In the default single-host mode, all three components run on localhost:

```
┌─────────────────────────────────────────────────────┐
│                    localhost                         │
│                                                     │
│  ┌────────────────┐    ┌──────────────┐             │
│  │ traffic_client │───▶│ proxy_server │──┐          │
│  │ (load gen)     │    │  :8081       │  │          │
│  └────────────────┘    └──────────────┘  │          │
│                                          │          │
│                        ┌──────────────┐  │          │
│                        │content_server│◀─┘          │
│                        │  :8082       │             │
│                        └──────────────┘             │
└─────────────────────────────────────────────────────┘
```

This mode is ideal for CPU and memory profiling of the proxy workload on a
single server, eliminating network variability.

## Distributed Topology

For NIC saturation and multi-host evaluation, each role runs on a separate host:

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Client Host │     │  Proxy Host  │     │ Server Host  │
│              │     │              │     │              │
│ traffic_     │────▶│ proxy_server │────▶│ content_     │
│ client       │     │  :8081       │     │ server :8082 │
└──────────────┘     └──────────────┘     └──────────────┘
```

## Multi-Instance Scaling

Each role supports multiple instances for horizontal scaling:

```
┌──────────────┐     ┌──────────────┐     ┌───────────────┐
│  Client Host │     │  Proxy Host  │     │  Server Host  │
│              │     │              │     │               │
│ client_1 ───────┬─▶│ proxy :8081 ─────┬─▶│ content :8082│
│ client_2 ───────┤  │ proxy :8091 ─────┤  │ content :8083│
│ client_3 ───────┘  │              │   └─▶│ content :8084│
└──────────────┘     └──────────────┘     └───────────────┘
```

Multiple content servers simulate backend fleet diversity. Multiple proxy
instances test proxy-layer horizontal scaling. Multiple clients generate
aggregate load beyond single-client capacity.

## Data Flow

1. **Startup:** run.sh starts content_server, then proxy_server, then waits
   for both to be listening via `ss -tlnp` port checks.

2. **Traffic generation:** traffic_client sends HTTP requests at the target RPS
   to proxy_server. Each connection multiplexes up to N concurrent streams (H2).

3. **Proxying:** proxy_server receives requests, forwards them to content_server,
   and returns responses. Tracks latency and retry metrics internally.

4. **Completion:** After the configured duration, traffic_client exits. run.sh
   sends SIGTERM to proxy_server and content_server.

5. **Metrics collection:** run.sh parses XLOG output from each binary's stderr
   and echoes structured key-value metrics to stdout for the CDNBenchParser.

## Metrics Pipeline

```
Binary stderr (XLOG)
       │
       ▼
  run.sh parses via grep -oP
       │
       ▼
  Structured stdout (key: value format)
       │
       ▼
  CDNBenchParser._parse_cdn_metrics()
       │
       ▼
  Benchpress metrics JSON
```

## Process Lifecycle

```
run.sh
  │
  ├─ Start content_server (background)
  ├─ Start proxy_server (background)
  ├─ Health check: wait for ports via ss
  ├─ Run traffic_client (foreground, captures exit code)
  ├─ SIGTERM → proxy_server
  ├─ SIGTERM → content_server
  ├─ Parse stderr files → structured stdout
  └─ Exit with traffic_client's exit code

  trap cleanup EXIT ERR
    └─ Kills any remaining background processes
    └─ Removes temp stderr files
```

## Protocol Support

- **H2 (default):** HTTP/2 cleartext (h2c). Uses stream multiplexing for high
  concurrency over fewer TCP connections.
- **H1:** HTTP/1.1. One request per connection at a time (no multiplexing).
  Useful for baseline comparison.
