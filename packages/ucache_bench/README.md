<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# UCacheBench

UCacheBench is DCPerf's distributed data-caching benchmark and the successor to
TaoBench. It models a look-aside cache service with a CacheLib server and an
open-loop mcrouter client. The benchmark reports delivered QPS, latency, cache
hit ratio, request accounting, and warmup health over an exact measurement
window.

The single supported Benchpress job is **`ucache_bench_memory`**. `production`
and `extreme` are hardware-autosizer modes for that job, not separate jobs.

## System requirements

### Machine topology

A distributed run needs one server machine and one or more client machines:

- **Production** uses exactly one physical client host and targets approximately
  60% server CPU utilization.
- **Extreme** targets approximately 80% server CPU utilization and may use
  additional physical clients when the generated process count requires them.
- The server is the system under test. Client machines must retain enough CPU,
  memory, network, and source-port headroom to deliver the requested open-loop
  rate without drops.

Place the server and clients on the same low-latency network. They must be able
to resolve and reach each other over the configured data and administration
ports. The default data port is `11212`; coordinated runs normally assign a
separate administration port. Ensure the process file-descriptor limit is above
the generated connection count.

Use dedicated hosts for comparable performance results. Keep CPU frequency,
turbo, NUMA, and other host policies stable across runs, and avoid unrelated
workloads during warmup and measurement.

### Supported software

The source installer supports:

- x86-64 and AArch64;
- Ubuntu and Debian;
- CentOS, RHEL, and Fedora;
- at least 4 GiB of usable memory on the server.

Run the installer as `root` or with equivalent package-install privileges and
internet access. It invokes the system package manager and builds pinned
open-source dependencies. Install the package on the server and on every client
host; each role uses package-local binaries. The shell examples below also use
`jq` to construct role-specific JSON.

The installer caps build parallelism at 16 jobs to reduce memory pressure. Set
`NPROC` to request a lower limit on smaller machines.

## Installation

From the DCPerf repository root, install the only supported job:

```bash
./benchpress_cli.py install ucache_bench_memory
```

Packaged deployments that expose `./benchpress` can use the equivalent command:

```bash
./benchpress install ucache_bench_memory
```

The installer downloads pinned dependencies, builds the server and client,
runs a localhost traffic smoke test, and installs:

```text
benchmarks/ucache_bench/server/ucachebench_server
benchmarks/ucache_bench/client/ucachebench_client
benchmarks/ucache_bench/client/bind_source.so
```

To reinstall after changing the package or toolchain:

```bash
./benchpress_cli.py install -f ucache_bench_memory
```

## Recommended workflow

Run autosizing on the **server under test** so CPU affinity, cgroup memory, and
NUMA detection describe the measured machine. The examples below use a
production run; use `VARIANT=extreme` for the extreme topology.

### 1. Choose a calibration point and generate parameters

If no prior measurement exists, ask the autosizer for a conservative seed from
a measured baseline request latency:

```bash
export VARIANT=production
export BASELINE_LATENCY_US=1000  # replace with the measured baseline
SEED_RECOMMENDATION="$(
  ./packages/ucache_bench/autosize.sh \
    --variant="$VARIANT" \
    --baseline-latency-us="$BASELINE_LATENCY_US"
)"
export AGGREGATE_QPS="$(
  jq -r '.derived.effective_aggregate_qps' <<<"$SEED_RECOMMENDATION"
)"
```

The seed is a first experiment, not a capacity estimate. If a prior calibrated
point exists, set `AGGREGATE_QPS` to that value instead. Generate one flat
parameter object for the selected point:

```bash
PARAMS="$(
  ./packages/ucache_bench/autosize.sh \
    --variant="$VARIANT" \
    --aggregate-qps="$AGGREGATE_QPS" \
    --params-only
)"
```

The autosizer divides aggregate QPS across all client processes. Derive the
required host and process counts from the same object:

```bash
CLIENT_HOSTS="$(jq -r '.num_client_hosts' <<<"$PARAMS")"
PROCESSES_PER_HOST="$(jq -r '.num_source_ips + 1' <<<"$PARAMS")"
TOTAL_CLIENT_PROCESSES="$((CLIENT_HOSTS * PROCESSES_PER_HOST))"
printf 'hosts=%s processes_per_host=%s total_processes=%s\n' \
  "$CLIENT_HOSTS" "$PROCESSES_PER_HOST" "$TOTAL_CLIENT_PROCESSES"
```

Production must report one physical client host. Extreme uses eight processes
per host and may report more than one host.

### 2. Build role-specific JSON

Choose a reachable IPv6 address for the server and a free administration port.
The source-binding interposer requires an IPv6 destination. The data port
defaults to `11212`.

```bash
export SERVER_HOST=2001:db8::10  # replace with the server's reachable IPv6 address
export ADMIN_PORT=11213

SERVER_PARAMS="$(
  jq -c \
    --argjson admin_port "$ADMIN_PORT" \
    --argjson num_clients "$TOTAL_CLIENT_PROCESSES" \
    '. + {admin_port: $admin_port, num_clients: $num_clients}' \
    <<<"$PARAMS"
)"
CLIENT_PARAMS="$(
  jq -c \
    --arg server_host "$SERVER_HOST" \
    --argjson admin_port "$ADMIN_PORT" \
    '. + {server_host: $server_host, admin_port: $admin_port}' \
    <<<"$PARAMS"
)"
```

`num_clients` is the total number of client **processes**, not physical hosts.
The same nonzero administration port must be present in the server and every
client parameter object when process ramping is enabled.

Before starting the blocking server role, persist the generated client JSON and
process count, then copy both files to every reported client host using the same
mechanism you use to deploy the benchmark:

```bash
printf '%s\n' "$CLIENT_PARAMS" >ucache-client-params.json
printf '%s\n' "$PROCESSES_PER_HOST" >ucache-processes-per-host
```

### 3. Start the server role

Start the server first. Use `-k perf` when server telemetry is needed for
calibration and acceptance:

```bash
./benchpress_cli.py run ucache_bench_memory -r server \
  -i "$SERVER_PARAMS" -k perf
```

### 4. Start every client process

On every reported client host, configure `PROCESSES_PER_HOST` local IPv6
addresses: its primary address plus the `num_source_ips` additional addresses
requested by the autosizer. Write those addresses, one per line, to
`ucache-client-source-ips.txt`. Then run this block on every client host at
approximately the same time:

```bash
CLIENT_PARAMS="$(<ucache-client-params.json)"
PROCESSES_PER_HOST="$(<ucache-processes-per-host)"
mapfile -t CLIENT_SOURCE_IPS <ucache-client-source-ips.txt

if ((${#CLIENT_SOURCE_IPS[@]} < PROCESSES_PER_HOST)); then
  echo "not enough configured client source addresses" >&2
  exit 1
fi

BIND_SOURCE="$PWD/benchmarks/ucache_bench/client/bind_source.so"
run_clients() {
  local -a pids=()
  local i pid client_status=0

  for ((i = 0; i < PROCESSES_PER_HOST; ++i)); do
    (
      LD_PRELOAD="$BIND_SOURCE" \
      BIND_ADDRESS="${CLIENT_SOURCE_IPS[$i]}" \
        ./benchpress_cli.py run ucache_bench_memory -r client \
          -i "$CLIENT_PARAMS"
    ) >"ucache-client-$i.log" 2>&1 &
    pids+=("$!")
  done

  for pid in "${pids[@]}"; do
    wait "$pid" || client_status=1
  done
  return "$client_status"
}
run_clients
```

Each invocation starts one client process, and `open_loop_qps` is already a
per-process rate. Do not divide it again. The source addresses must already be
configured on that client host. The shipped interposer also accepts a
comma-separated `BIND_ADDRESSES` pool, but neither Benchpress nor `run.py`
enables source-address binding automatically. The administration protocol
coordinates the process ramp and shared measurement boundary across hosts.
Keep the uniquely named `ucache-client-<index>.log` files: concurrent clients
share the package-local `client_output.log`, so that file alone is not reliable
per-process evidence.

### 5. Calibrate QPS

Measure server CPU and generator health at each aggregate-QPS point. Scale the
next point by `target_cpu / observed_cpu`, bounded to `[0.67x, 1.50x]` per step.
Continue until points bracket 60% CPU for production or 80% for extreme, then
interpolate or bisect. Stop and report the target as unattainable if client CPU,
dropped arrivals, timeouts, or protocol errors become limiting first.

The package does not require a particular cluster scheduler. Another launcher
is valid if it preserves the generated host count, process count, proxy count,
connections, per-process QPS, and coordination parameters.

## Hardware sizing model

Let `M` be usable memory in MiB and `P` be physical cores. The principal rules
are:

```text
reserve_mib = min(32768, floor(M / 2))
cache_mib = floor_to_64(min(0.75 * (M - reserve_mib), 1048576))

production_processes = min(64, max(16, round_up_4(P / 8)))
extreme_processes = min(64, round_up_8(max(16, P / 6)))
production_client_hosts = 1
extreme_client_hosts = ceil(extreme_processes / 8)
```

Cache sizing reserves memory for the operating system, allocator, network
stack, and benchmark overhead, while the 1 TiB ceiling bounds the working set.
Process counts provide enough independent open-loop pacers without letting
client scheduling overhead dominate.

Proxy fanout uses physical cores, SMT state, and usable memory. It is rounded to
a multiple of four and bounded between 4 and 80. Connection demand uses:

```text
destinations_per_process = min(13250, 11000 + 64 * P)
target_total_connections =
    min(220000, max_variant_processes * destinations_per_process)
```

The total is rounded up to each variant's process-and-proxy quantum. Controlled
A/B testing showed that forcing every system to the connection cap can change
protocol behavior, so the generated topology retains the lower hardware-scaled
target when appropriate.

NUMA interleave is enabled only when hardware detection finds more than one
memory-bearing NUMA node. The remaining server-resource, proxy, connection,
workload, and calibration formulas are documented in
[SIZING.md](SIZING.md).

## Runtime contract

Generated open-loop configurations use:

- 60 seconds for connection activation;
- 64 seconds for the coordinated process ramp;
- 60 seconds for full-load stabilization;
- 720 seconds of adaptive warmup;
- one exact 240-second measurement window.

Ramp and stabilization traffic are outside the measurement window. The client
keeps one scheduled arrival per wire RPC, disables miss refill by default, and
paces arrivals independently of completions. Accepted callbacks must drain
within a bounded deadline before results are valid.

Aggregate QPS is calibrated toward 60% server CPU for production and 80% for
extreme. The initial rate can be estimated from logical CPUs and baseline
latency; subsequent points adjust QPS by the ratio of target to observed CPU,
with bounded step sizes. Do not infer server capacity from the hardware model
alone.

## Result reporting and acceptance

Benchpress prints a JSON summary and stores run artifacts under the normal
`benchmark_metrics_<run_id>` directory. The parser exposes delivered `qps`,
duration, GET/SET counts and errors, hit ratio, latency percentiles, and warmup
summary fields when present.

Strict acceptance also uses evidence that is not promoted into the JSON summary:
the uniquely redirected per-process Benchpress logs for generator drops,
timeout/TKO replies, local rejections, callback draining, and accounting; the
server's `MEASUREMENT_WINDOW` line for exact wall-clock boundaries; and server telemetry
for CPU and microarchitecture behavior. Run the server role with `-k perf` or
collect equivalent telemetry, then slice it to the emitted measurement window.

Accept a result only when all of the following hold:

- the exact 240-second measurement window completed;
- delivered QPS reached the calibrated target;
- protocol errors, timeout/TKO replies, and local rejections are zero;
- request accounting is conserved and accepted callbacks drained cleanly;
- late-plus-cap drops are below 0.1%;
- externally measured server CPU is within five percentage points of 60% for
  production or 80% for extreme;
- the server was not limited by memory pressure, I/O wait, client saturation,
  source-port exhaustion, or network placement;
- CPU and microarchitecture behavior remain aligned with the intended workload.

These conditions are an acceptance policy, not all parser-enforced gates. QPS
alone is not sufficient evidence. Record the generated topology, cache size,
connection count, software revision, host policy, raw validation counters, and
exact measurement interval with every accepted result.

## Local smoke and development

The installer runs a localhost smoke test automatically. For a small manual
smoke, start the server in one terminal:

```bash
./packages/ucache_bench/run.py server \
  --port=11212 \
  --memory-mb=1024 \
  --hash-power=20 \
  --rpc-io-threads=1 \
  --real
```

Then start the client in another terminal:

```bash
./packages/ucache_bench/run.py client \
  --server-host=127.0.0.1 \
  --server-port=11212 \
  --warmup-seconds=5 \
  --duration-seconds=10 \
  --key-count=10000 \
  --num-proxies=1 \
  --num-threads=1 \
  --real
```

The wrapper checks binary presence, child exit status, positive operations, and
basic accounting. It does not enforce every benchmark-acceptance condition;
inspect the raw client summary and server telemetry before accepting a result.

## Source build against existing dependencies

When pinned dependencies are already available in a staging prefix:

```bash
cmake -S packages/ucache_bench -B /tmp/ucachebench-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/staging \
  -DCMAKE_MODULE_PATH=/path/to/fbcode_builder/CMake \
  -DSTAGING_DIR=/path/to/staging \
  -DDEPS_DIR=/path/to/dependency-sources \
  -DTHRIFT1=/path/to/staging/bin/thrift1 \
  -DBUILD_TESTING=ON
cmake --build /tmp/ucachebench-build --parallel 16
ctest --test-dir /tmp/ucachebench-build \
  -R '^ucachebench_oss_smoke$' --output-on-failure
cmake --install /tmp/ucachebench-build --prefix /path/to/install
```

CMake generates Thrift outputs in the build directory and does not rewrite the
checked-out Carbon-generated sources.

## Troubleshooting

### Installation runs out of memory

Set `NPROC` to a lower value before installation. The installer already caps
parallelism at 16, but memory-constrained systems may require fewer jobs:

```bash
NPROC=8 ./benchpress_cli.py install -f ucache_bench_memory
```

### Clients cannot activate all connections

Verify server address resolution, data and administration ports, firewall
policy, process file-descriptor limits, and source-port availability. Keep the
connection ramp enabled; do not accept a run that enters measurement without a
clean stable warmup tail.

### Delivered QPS is below target

Check client CPU utilization, open-loop late/cap drops, network throughput,
source-port pressure, and per-process load balance. Lower the calibration point
or add the client hosts prescribed by the extreme topology rather than
extrapolating server capacity.

### Server CPU or microarchitecture behavior is unstable

Use dedicated hosts and a consistent CPU-frequency, turbo, NUMA, and interrupt
policy. Confirm that ramp, stabilization, warmup, and measurement intervals are
not mixed when collecting telemetry.

## Additional cache modes

The released reference workload is the memory-only `ucache_bench_memory` job.
For information about CacheLib memory and Navy configuration concepts, see
[CACHE_MODES.md](CACHE_MODES.md); those concepts do not define additional
supported UCacheBench jobs.
