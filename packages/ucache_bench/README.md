# UcacheBench

UcacheBench is a distributed CacheLib server and mcrouter client workload. The
public package includes the source installer, the `ucache_bench_memory`
Benchpress job, the runtime wrapper, and a hardware autosizer.

## Install

The installer supports x86-64 and AArch64 on Ubuntu/Debian and
CentOS/RHEL/Fedora. It downloads pinned open-source dependencies, limits build
parallelism to reduce memory pressure, builds UcacheBench, and runs a localhost
traffic smoke before installing the binaries.

```bash
./benchpress install ucache_bench_memory
```

The installed files are:

```text
benchmarks/ucache_bench/server/ucachebench_server
benchmarks/ucache_bench/client/ucachebench_client
benchmarks/ucache_bench/client/bind_source.so
```

To run the installer directly:

```bash
sudo ./packages/ucache_bench/install_ucache_bench.sh
```

Set `NPROC` to request fewer build jobs. The installer caps the value at 16.

### Build against existing dependencies

When the pinned dependencies are already available in a staging prefix:

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

## Local smoke and development run

The installer runs the localhost smoke automatically. For a manual small run,
start the server in one terminal:

```bash
./packages/ucache_bench/run.py server \
  --port=11212 \
  --memory-mb=1024 \
  --hash-power=20 \
  --rpc-io-threads=1 \
  --real
```

Then run the client in another terminal:

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

The wrapper exits nonzero for a failed child process, timeout, missing binary,
zero-operation result, inconsistent accounting, or a result with no successful
protocol response.

## Generate a starting configuration

The autosizer accepts affinity-visible logical CPUs, distinct physical cores,
usable memory, and a load variant. It uses generic topology classes and never
infers server QPS from hardware.

```bash
# Inspect the locally detected CPU and memory. No runnable QPS is emitted.
./packages/ucache_bench/autosize.sh --variant=production

# Supply a measured aggregate rate to obtain runnable benchmark parameters.
./packages/ucache_bench/autosize.sh \
  --aggregate-qps=<measured-qps> \
  --variant=production \
  --params-only
```

Without `--aggregate-qps`, the full output omits `open_loop_qps` and sets
`calibration.required=true`. `--params-only` then fails. A
`--baseline-latency-us` value can generate a conservative first point, but that
point still requires calibration.

The model uses only a few hardware relationships:

```text
substantial_smt = (logical_cpus - physical_cores) >= 0.5 * physical_cores
effective_cores = physical_cores
                + 0.25 * min(physical_cores, logical_cpus - physical_cores)
base_reserve_mib = min(32768, floor(usable_memory_mib / 2))
cache_mib = floor_to_64(min(0.75 * (usable_memory_mib - base_reserve_mib),
                            1048576))
rpc_io_threads = logical_cpus
production_processes = min(64, max(16, round_up_4(physical_cores / 8)))
extreme_processes = min(64, round_up_8(max(16, physical_cores / 6)))
```

Cache sizing follows TaoBench's simple 75% memory rule after reserving up to
32 GiB for the operating system and benchmark overhead. On machines below
64 GiB, the reserve is half of visible memory so the model remains usable on
small hosts. The 1 TiB ceiling prevents unbounded working sets.

Hash power follows cache-capacity boundaries. Production rounds logical CPUs
per process to the nearest multiple of four proxies and caps each process at 16 to
bound its thread footprint; the cap can leave very large synthetic inputs below
one proxy EventBase per logical CPU. Production places every process on one
client host. Extreme retains its separate stress-topology fanout. The one-host production invariant is covered for both T2 VNC inputs
under consideration. For 192 physical cores / 384 logical CPUs, the equations
produce `T=24`, `N=16`, `H=1`; for 248 physical cores / 496 logical CPUs, they
produce `T=32`, `N=16`, `H=1`. These are equation-level topology checks only,
not VNC hardware-acceptance claims; a full run is still required to prove that
one physical client can deliver the calibrated QPS without errors or drops.
Connection demand keeps its simple core-scaled equation because the CPL A/B
showed that forcing 220,000 connections changed protocol behavior. The target
uses `min(13250, 11000 + 64 * physical_cores)` destinations per process,
multiplied by the larger variant process count and capped at 220,000, then
rounds up to each variant's process-and-proxy quantum. Client thread count,
measurement in-flight depth, the lower warmup cap, warmup, duration, timeout,
and open-loop safety limits are fixed workload defaults rather than fitted
hardware equations. NUMA interleave is enabled only when hardware detection
finds more than one memory-bearing NUMA node. Latency reporting uses bounded,
cache-line-isolated per-worker priority sampling followed by a process-wide
merge, keeping one deterministic full-window reservoir without shared hot-path
locks or multi-gigabyte vector growth during high-QPS runs.

For the complete server-resource, process, proxy, connection, workload, and
calibration formulas, see [SIZING.md](SIZING.md).

The generated workload keeps one open-loop arrival per wire RPC, disables miss
refill, enables fiber request handling, and uses the packaged traffic
distribution. Warmup admits at most `max_inflight` physical requests per
worker/client and holds each slot through Carbon callback exit. Open-loop
measurement uses the separate per-proxy outstanding cap and avoids allocating a
second response-timeout timer for every RPC. At each phase boundary, logical
waiters are cancelled and accepted mcrouter callbacks receive a bounded drain
before measurement proceeds or results are reported. When
autosizing has a resolved open-loop QPS, multi-client runs also stagger process
traffic starts
before a full 240-second measurement window; ramp traffic is excluded, while
total connection count and steady-state offered QPS are unchanged.
Calibration-only output with no resolved QPS omits the process ramp as well as
the open-loop rate.

## Run on multiple client hosts

The autosizer reports an orchestration-neutral topology:

```text
total client processes = num_client_hosts * (num_source_ips + 1)
processes per client host = num_source_ips + 1
```

Start the server with the total client-process count, then divide the aggregate
open-loop rate evenly across those client processes. Launch the reported number
of processes on each client host. When multiple processes share a host, bind
each process to a distinct source address if the required connection count
would otherwise exhaust a single address's port space.

The package deliberately does not require a particular cluster scheduler. Use
Benchpress roles, direct `run.py` commands, or another launcher while preserving
the emitted process, connection, and per-process QPS values.

## Calibrate and accept a result

Use the generated rate only as the start of an exponential bracket. Adjust with
`next_qps`, bracket the target CPU, then interpolate or bisect. Report the target
as unattainable if client saturation, dropped arrivals, timeouts, or protocol
errors appear first; do not extrapolate server capacity.

Check primary output before accepting a result:

- the configured measurement window completed;
- total operations and delivered QPS are positive;
- GET and SET accounting is internally consistent and errors are zero;
- clients retain load-generation headroom without sustained drops;
- CPU is in the requested band and memory pressure or I/O wait does not explain
  the result;
- the client topology, cache size, and connection count are recorded.

For memory-only and Navy configuration details, see
[CACHE_MODES.md](CACHE_MODES.md).
