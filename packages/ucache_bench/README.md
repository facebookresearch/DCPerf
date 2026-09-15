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

The model uses only symbolic topology and memory relationships:

```text
substantial_smt = (logical_cpus - physical_cores) >= 0.5 * physical_cores
effective_cores = physical_cores
                + 0.25 * min(physical_cores, logical_cpus - physical_cores)
cache_fraction = clamp(0.40, 0.80,
                       0.40 + 0.125 * log2(usable_memory_mib / 65536))
reserve_mib = max(2048, ceil(0.20 * usable_memory_mib))
cache_mib = floor_to_64(min(usable_memory_mib * cache_fraction,
                            usable_memory_mib - reserve_mib,
                            1048576))
rpc_io_threads = logical_cpus
production_processes = min(64, round_up_4(max(16, physical_cores / 8)))
extreme_processes = min(64, round_up_8(max(16, physical_cores / 6)))
```

Hash power follows cache-capacity boundaries. Proxy fanout uses generic SMT,
memory-rich, and high-core branches. The server connection target is independent
of load variant and is aligned to a common multiple of both variants' process
and proxy counts, so production and extreme emit exactly the same total
connections while keeping integral fanout and at most 32,768 destinations per
process. The variants differ through offered load and client host/process
placement. Client thread count, in-flight depth, warmup, duration, timeout, and
open-loop safety limits are workload defaults rather than fitted hardware
equations.

For the complete server-resource, process, proxy, connection, workload, and
calibration formulas, see [SIZING.md](SIZING.md).

The generated workload keeps one open-loop arrival per wire RPC, disables miss
refill, enables fiber request handling, and uses the packaged traffic
distribution. When autosizing has a resolved open-loop QPS, multi-client runs
also stagger process traffic starts before a full 240-second measurement window;
ramp traffic is excluded, while total connection count and steady-state offered
QPS are unchanged. Calibration-only output with no resolved QPS omits the process
ramp as well as the open-loop rate.

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
