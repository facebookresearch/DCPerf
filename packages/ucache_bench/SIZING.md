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

The formulas below use `clamp(low, high, value)`, which returns `low` when
`value` is below the lower bound, `high` when it is above the upper bound, and
`value` otherwise. `floor_to_q`, `round_up_q`, and half-up
`round_nearest_q` round to a multiple of `q` in their usual directions.

The cache and connection coefficients are workload-level calibration constants.
They are applied uniformly to all machines; only detected topology and usable
memory select an output.

## Cache and server resources

Cache sizing uses TaoBench's 75% memory rule after a fixed operating-system and
benchmark allowance:

```text
base_reserve_mib = min(32768, floor(M / 2))
cache_mib = floor_to_64(min(0.75 * (M - base_reserve_mib), 1048576))
key_count = floor(cache_mib * 1048576 / average_item_bytes)
```

The reserve reaches 32 GiB on machines with at least 64 GiB and is half of
visible memory on smaller machines. Applying the 75% factor after that reserve
leaves additional proportional headroom for allocator and per-item metadata.
The cache remains capped at 1 TiB, and `average_item_bytes` defaults to `1024`.
The model requires at least 4 GiB of usable memory. Cache sizing does not depend
on SMT.

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
server_numa_interleave = 1 if NUMA memory nodes > 1, otherwise 0
hashtable_lock_power = 24
cachelib_num_shards = 524288
```

NUMA interleave is enabled from detected topology rather than platform identity.
It is used only when more than one NUMA node has memory, preventing a large
cache from exhausting one node while other allowed nodes remain idle.

## Client processes and hosts

Let `T` be total client processes, `H` client hosts, and `R` processes per host.
Process counts are bounded to avoid unhelpful scheduler fanout:

```text
production:
  T = min(64, max(16, round_up_4(P / 8)))
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

Production uses one physical client by construction. For the two T2 VNC inputs
under consideration, 192 physical cores / 384 logical CPUs produce `T=24`,
`N=16`, `H=1`, while 248 physical cores / 496 logical CPUs produce `T=32`,
`N=16`, `H=1`. Unit tests lock down both equation-level cases. They do not
constitute VNC hardware acceptance: a full run is still required to prove that
one physical client can deliver the calibrated QPS without errors or drops
because the autosizer never predicts load-generator headroom.

When several processes share a host, each process needs a distinct source
address if one address cannot provide the required destination count.

## Proxy scheduler fanout

Let `N` be proxies per process. Production uses one short capacity equation:

```text
production:
  N = clamp(4, 16, round_nearest_4(L / T))
```

This rounds to the closest one proxy EventBase per logical CPU on the supported
production topologies, avoiding a large client-thread jump when the ratio is just
above a multiple of four. The 16-proxy cap bounds each process's thread
footprint on larger inputs. Extreme retains its larger stress-topology fanout:

```text
extreme without substantial SMT:
  requested = max(20, P / 4)

extreme with substantial SMT and M >= 512 * 1024:
  requested = P / 2

extreme with other substantial SMT and P >= 64:
  requested = 0.68 * P

extreme with other substantial SMT:
  requested = max(20, 0.75 * P)

N = clamp(4, 80, round_nearest_4(requested))
```

## Connections

Connections remain hardware-scaled because the CPL A/B showed that forcing a
fixed 220,000 changed protocol behavior. The equation is bounded and shared by
both variants:

```text
T_max = max(T_production, T_extreme)
destinations_per_process = min(13250, 11000 + 64 * P)
target_connections = min(220000, T_max * destinations_per_process)

Q_v = T_v * N_v
total_connections_v = round_up_to_Q_v(target_connections)
variant_max_v = Q_v * floor(32768 / N_v)
additional_fanout_v = total_connections_v / Q_v - 1
```

Rounding preserves integral fanout and never exceeds 32,768 destinations per
process. Sizing fails if a topology cannot represent the aligned target.

## Workload defaults

These settings define benchmark semantics and do not vary with hardware:

```text
num_threads = 8
max_inflight = 150
warmup_max_inflight = 32
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

Open-loop protection uses fixed workload bounds rather than a hardware profile:

```text
production: open_loop_max_outstanding = 8192
extreme:    open_loop_max_outstanding = 8192
both:       open_loop_max_lateness_us = 500000
            failures_until_tko = 12
```

One open-loop arrival remains one wire RPC, fiber request handling remains
enabled, and the packaged workload distribution is used. Warmup admits at most
32 physical requests per worker/client, retaining each slot until its Carbon
callback exits; with the default eight workers this bounds the process to 256
warmup requests. Startup connection recovery may report transient errors, so
acceptance requires the final `min(60 seconds, warmup duration)` tail to be
error-free rather than the entire connection-establishment interval. Measurement
uses the separate 8,192-request per-proxy cap; the 500 ms lateness bound allows
brief scheduler recovery while that cap remains the hard pressure limit.
Generator late/cap drops are acceptable only while their combined rate remains
below 0.1%, the built-in `generator_limited=false` criterion; protocol errors,
timeouts, local rejections, and accounting violations remain zero-tolerance.
Open-loop requests do not allocate a second
response-timeout timer for every RPC. Latency percentiles use deterministic
priority sampling over every completed request, capped at 16,384 samples per
process. Cache-line-isolated workers retain their candidates without
synchronization, and the process keeps the globally lowest priorities, preserving full-window,
request-proportional pseudorandom sampling without a shared hot-path lock. At each phase boundary, logical waiters
are cancelled and accepted mcrouter callbacks drain for up to
`physical_drain_timeout_seconds` before measurement proceeds or results are
reported. Completion-driven mode retains its per-request timeout. When sizing
has a resolved open-loop QPS, it
also emits a 64-second process ramp: multi-client traffic starts are spread
before the full 240-second measurement window. Ramp traffic is excluded from
reported counters; total connections and steady-state offered-QPS semantics are
unchanged. Calibration-only output with no resolved QPS omits both
`open_loop_qps` and `process_ramp_seconds`.

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
