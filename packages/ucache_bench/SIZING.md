# UcacheBench hardware sizing

`autosize` derives a runnable topology from affinity-visible CPU topology and
usable memory. The five platforms used for the published generation-correlation
study have validated profiles that reproduce the accepted benchmark
configuration. Other machines use a generic fallback and require calibration.

The autosizer does not predict server capacity. Offered load must come from an
explicit measurement or from a latency-based seed followed by measurement.

```bash
./packages/ucache_bench/autosize.sh \
  --variant production \
  --aggregate-qps <measured-qps> \
  --params-only
```

Without `--params-only`, the tool emits diagnostics plus an
orchestration-neutral `benchmark_params` object. The diagnostics identify the
selected `sizing_profile`; it is `generic` when no validated profile matches.

## Inputs

The inputs are:

- affinity-visible logical CPUs `L`;
- distinct physical cores `P`, found from visible thread-sibling groups;
- CPU model from `/proc/cpuinfo`;
- usable memory `M` in MiB;
- load variant `production` or `extreme`.

Usable memory is the minimum of `MemTotal` and every finite `memory.max` or
`memory.high` limit from the current cgroup v2 path through its ancestors.
Explicit `--logical-cpus`, `--physical-cores`, `--cpu-model`, and
`--memory-mib` values override detection.

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

## Validated profiles

A profile matches CPU model, exact CPU topology, and a bounded memory range. The
memory range accepts normal firmware/kernel differences while preventing an
unrelated CPU or restricted cgroup from selecting a validated profile. Profile
selection also verifies that the cache plus reserve fits. These values are the
exact configurations used for the clean 60% and 80% correlation rows.

| Profile | Hardware match (CPU model, `L`, `P`, memory) | Cache / keys | Connections | Production clients | Extreme clients |
|---|---|---|---:|---|---|
| `T1_CPL` | 8321HC, 52, 26, 60-68 GiB | 24,000 MiB / 24M | 200,960 | 2 hosts x 8 processes x 20 proxies | 2 x 8 x 20 |
| `T1_MLN` | 7D13, 72, 36, 60-68 GiB | 24,000 MiB / 24M | 212,352 | 2 x 8 x 28 | 2 x 8 x 28 |
| `T11_GRC_ARM` | Neoverse-V2/MIDR 0x41:0xd4f, 72, 72, 240-272 GiB | 160,000 MiB / 160M | 212,160 | 2 x 8 x 20 | 2 x 8 x 20 |
| `T1_BGM` | 9D64, 176, 88, 240-272 GiB | 170,000 MiB / 170M | 212,160 | 2 x 8 x 60 | 2 x 8 x 60 |
| `T2_TRN` | 9D25, 316, 158, 1001-1088 GiB | 820,000 MiB / 820M | 222,720 | 2 x 10 x 32 | 4 x 8 x 80 |

Connections divide evenly across every process and proxy. The resulting
`additional_fanout` values are:

| Profile | Production | Extreme |
|---|---:|---:|
| `T1_CPL` | 627 | 627 |
| `T1_MLN` | 473 | 473 |
| `T11_GRC_ARM` | 662 | 662 |
| `T1_BGM` | 220 | 220 |
| `T2_TRN` | 347 | 86 |

The accepted measured loads are supplied through `--aggregate-qps`; they are
not inferred from the profile:

| Profile | Production aggregate / per process | Extreme aggregate / per process |
|---|---:|---:|
| `T1_CPL` | 510K / 31,875 | 620K / 38,750 |
| `T1_MLN` | 608K / 38,000 | 820K / 51,250 |
| `T11_GRC_ARM` | 1.248M / 78,000 | 1.744M / 109,000 |
| `T1_BGM` | 2.112M / 132,000 | 2.896M / 181,000 |
| `T2_TRN` | 3.850M / 192,500 | 4.800M / 150,000 |

## Generic fallback

An unmatched machine retains formula-based sizing so new hardware can begin
calibration without an LSST-specific code change. Generic output is not an
accepted capacity point until it passes the benchmark gates.

### Cache and server resources

The fallback cache uses a bounded logarithmic memory-utilization curve:

```text
f = clamp(0.40, 0.80, 0.40 + 0.125 * log2(M / 65536))
reserve_mib = max(2048, ceil(0.20 * M))
cache_mib = floor_to_64(min(M * f, M - reserve_mib, 1048576))
key_count = floor(cache_mib * 1048576 / average_item_bytes)
```

The explicit reserve protects the operating system and benchmark processes,
and the cache remains capped at 1 TiB. `average_item_bytes` defaults to `1024`.
Validated profiles require that default because their key counts are part of the
measured contract; use generic sizing for a different item size.

Hash power depends on cache capacity:

```text
cache_mib <=  64 * 1024  -> hash_power = 28
cache_mib <= 256 * 1024  -> hash_power = 29
cache_mib <= 512 * 1024  -> hash_power = 30
otherwise                -> hash_power = 31
```

The following values are workload constants:

```text
rpc_num_acceptor_threads = 4
rpc_num_cpu_worker_threads = 1
hashtable_lock_power = 24
cachelib_num_shards = 524288
```

### Generic client topology

Let `T` be total client processes, `H` client hosts, and `R` processes per host:

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

Proxy count `N` is derived as follows and rounded to a multiple of four in the
range `[4, 80]`:

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

The generic connection anchor is 212,160, the central validated connection
count, rather than the previous unvalidated 300,000 floor. It is rounded upward
to a common multiple of the production and extreme process/proxy quanta:

```text
Q_production = T_production * N_production
Q_extreme = T_extreme * N_extreme
Q = lcm(Q_production, Q_extreme)
total_connections = round_up_Q(212160)
additional_fanout_v = total_connections / Q_v - 1
```

Sizing fails if alignment would exceed 32,768 destinations per process.
Validated profiles bypass this fallback and return their measured connection
counts exactly.

## Workload defaults

These settings define benchmark semantics:

```text
num_threads = 8
max_inflight = 150
warmup_seconds = 720
duration_seconds = 240
timeout_seconds = 2400
connection_ramp_seconds = 25
process_ramp_seconds = 64 when QPS is resolved
open_loop_refill_on_miss = 0
open_loop_max_outstanding = 4096
open_loop_max_lateness_us = 200000
failures_until_tko = 12
min_alloc_size = 64
enable_fibers = 1
enable_random_source_ip = 1
use_same_thread_client = 1
use_distribution = 1
distribution_config = "./packages/ucache_bench/traffic_dist.json"
```

The 64-second generated process ramp is the conservative post-validation policy.
It changes startup only; traffic during the ramp remains outside the full
240-second measurement window.

## Offered-load calibration

`--aggregate-qps` supplies a measured aggregate open-loop rate. It is divided
evenly across client processes and must be at least the process count. Without
it, `open_loop_qps` is omitted and the full JSON reports
`calibration.required=true`. `--params-only` fails in that state so a runnable
configuration cannot silently become completion-driven.

If only `--baseline-latency-us` is present, the tool emits a deliberately low
seed:

```text
seed_qps = max(1, floor(0.10 * rpc_io_threads * 1000000 / latency_us))
```

A latency seed still requires calibration. Calibrate with an exponential
bracket followed by interpolation or bisection:

1. Run the seed and verify positive delivered operations, zero protocol errors,
   and generator headroom.
2. Call `next_qps(current_qps, observed_cpu, target_cpu)`. It scales by
   `target_cpu / observed_cpu`, bounded to `[0.67x, 1.50x]` per step.
3. Continue until measurements bracket the target CPU, then interpolate or
   bisect.
4. Report `unattainable` if client CPU, dropped arrivals, timeouts, protocol
   errors, or memory pressure become limiting first.
