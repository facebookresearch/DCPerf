# UcacheBench hardware sizing

`autosize` derives a runnable topology from affinity-visible CPU topology and
usable memory. It does not predict server capacity. Offered load must come from
an explicit measurement or from a latency-based seed followed by measurement.

```bash
./packages/ucache_bench/autosize.sh \
  --variant production \
  --aggregate-qps <measured-qps> \
  --params-only
```

Without `--params-only`, the tool emits diagnostics plus an
orchestration-neutral `benchmark_params` object. With `--params-only`, it emits
only that object. Callers may map the reported process and host counts to any
launcher that preserves the topology.

## Inputs and notation

The inputs are:

- affinity-visible logical CPUs `L`;
- distinct physical cores `P`, found from visible thread-sibling groups;
- usable memory `M` in MiB;
- load variant `production` or `extreme`.

Usable memory is the minimum of `MemTotal` and every finite `memory.max` or
`memory.high` limit from the current cgroup v2 path through its ancestors.
Explicit `--logical-cpus`, `--physical-cores`, and `--memory-mib` values override
detection.

SMT is substantial when:

```text
L - P >= 0.5 * P
```

The diagnostic effective-core value discounts sibling threads:

```text
E = P + 0.25 * min(P, L - P)
```

`E` is reported for comparison only. Server RPC I/O threads use all visible
logical CPUs: `rpc_io_threads = L`.

The formulas below use `clamp(low, high, value)`, `floor_to_q`, `round_up_q`,
and half-up `round_nearest_q` in their usual meanings.

## Cache and server resources

The cache uses a logarithmic memory-utilization curve. The fraction grows by
`0.125` for each doubling above 64 GiB and is bounded between `0.40` and `0.80`:

```text
f = clamp(0.40, 0.80, 0.40 + 0.125 * log2(M / 65536))
reserve_mib = max(2048, ceil(0.20 * M))
cache_mib = floor_to_64(min(M * f, M - reserve_mib, 1048576))
key_count = floor(cache_mib * 1048576 / average_item_bytes)
```

The explicit reserve protects the operating system and benchmark processes,
and the cache remains capped at 1 TiB. `average_item_bytes` defaults to `1024`.
The model requires at least 4 GiB of usable memory.

Hash power depends only on cache capacity:

```text
cache_mib <=  64 * 1024  -> hash_power = 28
cache_mib <= 256 * 1024  -> hash_power = 29
cache_mib <= 512 * 1024  -> hash_power = 30
otherwise                -> hash_power = 31
```

The following values are workload constants rather than hardware profiles:

```text
rpc_num_acceptor_threads = 4
rpc_num_cpu_worker_threads = 1
hashtable_lock_power = 24
cachelib_num_shards = 524288
```

## Client processes and hosts

Let `T` be total client processes, `H` client hosts, and `R` processes per host.
Process counts are bounded to avoid unhelpful scheduler fanout:

```text
production:
  T = min(64, round_up_4(max(16, P / 8)))
  H = 1
  R = T

extreme:
  T = min(64, round_up_8(max(16, P / 6)))
  R = 8
  H = ceil(T / 8)
```

The output maps these values to:

```text
num_client_hosts = H
num_source_ips = R - 1
total client processes = H * (num_source_ips + 1)
```

When several processes share a host, each process needs a distinct source
address if one address cannot provide the required destination count.

## Proxy scheduler fanout

Let `N` be proxies per process. First compute the requested value:

```text
no substantial SMT:
  requested = max(20, P / 4)

substantial SMT and M >= 512 * 1024:
  production requested = P / 5
  extreme requested = P / 2

other substantial SMT and P >= 64:
  requested = 0.68 * P

other substantial SMT:
  requested = max(20, 0.75 * P)
```

Then:

```text
N = clamp(4, 80, round_nearest_4(requested))
```

Memory-rich systems can sustain a larger in-flight working set, so the
scheduler fanout may remain variant-specific there. The extreme variant uses
more proxies to expose concurrency, while production limits scheduler overhead.
This scheduler choice does not change the shared server connection count. Other
high-core SMT systems use an intermediate fanout independent of variant. The
final clamp also keeps every proxy representable within the per-process
destination limit.

## Connections

The server connection target is independent of load variant. Its density `d` is
per physical core:

```text
no substantial SMT:
  d = 550

substantial SMT and M >= 512 * 1024:
  d = 2550

other substantial SMT and P >= 64:
  d = 1500

other substantial SMT:
  d = 1400
```

Connections divide evenly across every process and proxy in both variants. Let
`T_v` and `N_v` be the process and proxy counts for variant `v`, then define:

```text
Q_production = T_production * N_production
Q_extreme = T_extreme * N_extreme
Q = lcm(Q_production, Q_extreme)

target_connections = P * d
aligned_connections = max(Q, round_nearest_Q(target_connections))
variant_max_v = Q_v * floor(32768 / N_v)
shared_max = floor_to_Q(min(variant_max_production, variant_max_extreme))
total_connections = min(aligned_connections, shared_max)
additional_fanout_v = total_connections / Q_v - 1
```

The common quantum makes `total_connections` identical across variants while
preserving integral fanout for every process and proxy. The shared cap keeps
each variant at or below 32,768 destinations per process. Production and
extreme therefore differ through offered load and client host/process placement,
not through the server connection target.

## Workload defaults

These settings define benchmark semantics and do not vary with hardware:

```text
num_threads = 8
max_inflight = 150
warmup_seconds = 720
duration_seconds = 240
timeout_seconds = 2400
connection_ramp_seconds = 25
open_loop_refill_on_miss = 0
min_alloc_size = 64
enable_fibers = 1
enable_random_source_ip = 1
use_same_thread_client = 1
use_distribution = 1
distribution_config = "./packages/ucache_bench/traffic_dist.json"
```

Open-loop protection differs by load variant:

```text
production: open_loop_max_outstanding = 1024
            open_loop_max_lateness_us = 1000
extreme:    open_loop_max_outstanding = 4096
            open_loop_max_lateness_us = 200000
```

One open-loop arrival remains one wire RPC, fiber request handling remains
enabled, and the packaged workload distribution is used.

## Offered-load calibration

`--aggregate-qps` supplies a measured aggregate open-loop rate. It is divided
evenly across client processes and must be at least the process count. Without
it, `open_loop_qps` is omitted and the full JSON reports
`calibration.required=true`. `--params-only` fails in that state so a runnable
configuration cannot silently become completion-driven.

If only `--baseline-latency-us` is present, the tool emits this deliberately low
seed:

```text
seed_qps = max(1, floor(0.10 * rpc_io_threads * 1000000 / latency_us))
```

A latency seed still reports `calibration.required=true`. It is a safe first
point, not a capacity estimate.

Calibrate with an exponential bracket followed by interpolation or bisection:

1. Run the seed and verify positive delivered operations, zero protocol errors,
   and generator headroom.
2. Call `next_qps(current_qps, observed_cpu, target_cpu)`. It scales by
   `target_cpu / observed_cpu`, bounded to `[0.67x, 1.50x]` per step.
3. Continue until measurements bracket the target CPU, then interpolate between
   the bracket points and use bisection when measurements are noisy.
4. Stop and report `unattainable` if client CPU, dropped arrivals, timeouts, or
   protocol errors become limiting first. Do not extrapolate a capacity value
   past that point.
