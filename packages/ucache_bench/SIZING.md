# UcacheBench Hardware Sizing

`autosize` generates the parameter object used by the current
`ucache_bench_debug` Automark adapter from the host's CPU topology, installed
memory, and one of two load variants:

- `production`: approximately 60% server CPU, one physical client host.
- `extreme`: approximately 80% server CPU, two or four physical client hosts.

```bash
# From an fbsource checkout
buck2 run @fbcode//mode/opt \
  fbcode//cea/chips/benchpress/packages/ucache_bench:autosize -- \
  --variant production --params-only

# From an installed Benchpress package
./packages/ucache_bench/autosize.sh --variant production --params-only
```

The output is a starting configuration, not permission to accept a result
without checking the measurement window, client accounting, and errors. The
`num_client_hosts` and `num_source_ips` topology currently requires
`ucache_bench_debug`, whose adapter creates and co-locates the client roles.
The standard Automark adapter does not yet implement that topology contract.

## Hardware detection

The generator uses the process CPU affinity rather than the machine-wide CPU
count. Physical cores are the unique `thread_siblings_list` groups intersected
with that affinity. This handles SMT off, SMT2, larger SMT factors, and cpusets
without assuming sibling IDs occupy the second half of the CPU range.

Installed memory comes from `MemTotal` in `/proc/meminfo`.

## Validated hardware anchors

The five measured LSSTs have exact calibration anchors. Matching uses logical
CPUs, physical cores, and memory with bounded tolerances, so ordinary firmware
or reserved-memory differences do not require per-LSST JSON files.

| LSST | Logical | Physical | Memory | Production clients | Extreme clients |
|---|---:|---:|---:|---:|---:|
| T2_TRN | 316 | 158 | 1,034,736 MiB | 1 | 4 |
| T1_BGM | 176 | 88 | 257,120 MiB | 1 | 2 |
| T11_GRC_ARM | 72 | 72 | 260,682 MiB | 1 | 2 |
| T1_MLN | 72 | 36 | about 62,000 MiB | 1 | 2 |
| T1_CPL | 52 | 26 | about 62,000 MiB | 1 | 2 |

For a matched anchor, the generator reproduces the measured cache size, hash
power, aggregate offered QPS, process count, proxy count, connection count, and
physical client count. The remaining production-like workload parameters are
shared.

## Formula fallback for new hardware

Unmatched hardware uses formulas derived from the measured matrix. The JSON
output reports `source: formula`; those results require calibration before they
become a new anchor.

### Cache capacity

Let `M` be installed memory in MiB. The fallback is a continuous, monotonic
piecewise-linear interpolation through the measured capacity points:

```text
M <= 48,000:       cache = 0.50 * M
48,000 < M <= 256,000:
                      cache = 24,000 + (M - 48,000) * 146,000 / 208,000
M > 256,000:       cache = 170,000 + (M - 256,000) * 650,000 / 778,736
```

The result is rounded to 1,000 MiB and capped at 80% of installed memory.
`key_count = 1000 * cache_mb`. Hash power is 28 through 32,000 MiB, 29 through
256,000 MiB, and 31 above 256,000 MiB.

### Offered load and client processes

The formulas use physical cores because the five-platform data shows that SMT
siblings do not provide another full core of cache-server throughput.
Production uses 16 processes, or 20 above 128 physical cores, on one host.
Extreme uses eight processes per physical client host: two hosts normally and
four above 128 physical cores.

Per-core offered QPS is selected by variant, substantial SMT coverage, and
core-count class. SMT contributes only when at least half of the visible
physical cores expose a sibling, avoiding a step change from one stray sibling:

| Variant | Topology | Offered QPS / physical core |
|---|---|---:|
| Production | SMT, at least 128 cores | 31,500 |
| Production | SMT, 64-127 cores | 28,500 |
| Production | Other | 21,000 |
| Extreme | no SMT | 24,000 |
| Extreme | SMT, at least 64 cores | 30,500 |
| Extreme | SMT, below 64 cores | 29,000 |

Aggregate QPS is rounded to 10K and divided across processes. One arrival is
one wire RPC; open-loop miss refills remain disabled.

### Proxies and connections

Proxy count is derived separately because it controls client scheduling as
well as connections:

- Less than 50% SMT sibling coverage: `round4(max(20, physical_cores / 4))`.
- SMT, at least 128 cores: 80.
- SMT, 64-127 cores: `round4(0.68 * physical_cores)`.
- Smaller SMT hosts: `round4(max(20, 0.75 * physical_cores))`.

Total connection density is 2,550 per physical core above 512 GiB, 550 on
systems without substantial SMT coverage, 1,500 on SMT systems with at least
64 cores, and 1,400 on smaller SMT systems. The total is capped at 32,768
proxy destinations per process before `additional_fanout` is derived.

### SMT-aware capacity estimate

For comparison with the correlation study, the generator reports:

```text
estimated_RRU = 2/3 + physical_cores / 15 + memory_GiB / 30
```

This fit describes the five measured LSSTs but does not replace an official RRU
or a benchmark result.

## Acceptance criteria

A formula-generated point should be promoted to a calibration anchor only when:

- the 240-second measurement window is within the intended CPU band;
- generator drops are at most 0.1%, with no timeouts or TKO collapse;
- server-aggregated delivered QPS contains real measurements;
- iowait and memory pressure do not explain the CPU result;
- the lower physical-client count is shown unable to reach the target cleanly;
- CPU placement, cache size, process count, and total connections are recorded.

Explicit command-line or Automark parameters remain the escape hatch for
controlled experiments; autosizing does not remove them.
