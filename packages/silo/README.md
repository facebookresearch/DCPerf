# Silo

[Silo](https://github.com/stephentu/silo) is a high-performance in-memory OLTP database from MIT (Tu et al., SOSP 2013). It scales to many cores using optimistic concurrency control (OCC) over [Masstree](https://github.com/kohler/masstree-beta), an efficient concurrent trie of B+-trees. Silo's `dbtest` benchmark binary runs TPC-C, YCSB, and a small set of microbenchmarks against the database.

This benchpress package builds `dbtest` from upstream at a pinned commit and wraps it with the standard install / cleanup / run interface.

> **Note:** Silo lives in `benchpress/config/benchmarks_internal.yml` / `jobs_internal.yml`. It is part of Meta's internal benchpress suite and not shipped in the public DCPerf release.

## System requirements

| Item | Detail |
|---|---|
| OS | RHEL/CentOS 9 (tested), Ubuntu 22.04 (untested) |
| CPU | Any x86_64 with multiple cores. Validated on 56-core Intel and AMD hosts. Silo is designed to scale to many cores. |
| Memory | At least 8 GB; ~5 GB per warehouse for TPC-C scale-factor sweeps |
| Compiler | GCC 11+ (the install script patches the upstream Makefile to tolerate modern `-Werror` defaults) |
| Network | Outbound HTTPS to github.com via `fwdproxy` (the install script wires this up automatically with `fwdproxy-config`) |
| Privileges | `sudo` for the initial `dnf install` of build dependencies |

The install script declares the dnf deps it needs, but for reference: `jemalloc-devel`, `numactl-devel`, `libdb-cxx-devel`, `mysql-devel`, `libaio-devel`, `openssl-devel`, `zlib-devel`, `autoconf`, `automake`, `libtool`, `fb-fwdproxy-config`.

## Install Silo

```bash
./benchpress_cli.py install silo_default
```

This (a) installs build dependencies via `dnf`, (b) clones `stephentu/silo` and its `masstree` submodule via `fwdproxy`, (c) builds `dbtest` in `MODE=perf` with `jemalloc`, and (d) places the binary at `./benchmarks/silo/dbtest`.

The install is idempotent — re-running re-clones and rebuilds, then overwrites the binary. To uninstall:

```bash
./benchpress_cli.py clean silo_default
```

## Run Silo

### Recommended jobs

| Job | Workload | Threads | Scale | Runtime |
|---|---|---|---|---|
| `silo_default` | TPC-C smoke | 1 | 1 | 1s |
| `silo_tpcc_1t` | TPC-C single-thread baseline | 1 | 1 | 30s |
| `silo_tpcc_allcores` | TPC-C all cores (`--num-threads 0` → `$(nproc)`) | host | 32 | 30s |
| `silo_tpcc_scale10` | TPC-C medium contention | 8 | 10 | 30s |

### YCSB workloads

The workload mix is passed via Silo's `--bench-opts '--workload-mix=READ,RMW,WRITE,SCAN'` (percentages summing to 100).

| Job | Mix | Notes |
|---|---|---|
| `silo_ycsb_a` | 50r / 50 update | Session-store style |
| `silo_ycsb_b` | 95r / 5 update | Photo-tagging style |
| `silo_ycsb_c` | 100r | Pure read; expect ~10M+ ops/sec/core in-memory |
| `silo_ycsb_e` | 95 scan / 5 insert | Threaded conversations style |
| `silo_ycsb_f` | 50r / 50 rmw | User-database style |

### Microbenchmark

| Job | Description |
|---|---|
| `silo_queue` | dbtest's `queue` workload — simplest KV insert/delete loop |

### Thread-count sweep

`silo_tpcc_sweep_{1,2,4,8,16,32,64}t` — one warehouse per worker thread (Silo recommends this ratio to avoid pathological abort rates). Run with:

```bash
for t in 1 2 4 8 16 32 64; do
    ./benchpress_cli.py run silo_tpcc_sweep_${t}t
done
```

### Pinning / NUMA control

`packages/silo/run.sh` honors three environment variables:

| Env var | Effect |
|---|---|
| `SILO_NUMA_NODE=0` | Wraps in `numactl --membind=0 --cpunodebind=0` (single-socket run) |
| `SILO_CPUS=0-27` | Wraps in `taskset -c 0-27` (specific CPU pinning) |
| `SILO_BIN=/path/to/dbtest` | Override the binary path |

Example: pin to NUMA node 0 for a single-socket TPC-C scan:

```bash
SILO_NUMA_NODE=0 ./benchpress_cli.py run silo_tpcc_allcores
```

## Reporting and measurement

The Silo parser (`benchpress/plugins/parsers/silo.py`) extracts these metric groups from `dbtest`'s stderr:

| Group | Fields |
|---|---|
| `throughput` | `agg_throughput`, `avg_per_core_throughput`, `agg_nosync_throughput`, `agg_persist_throughput`, `avg_per_core_persist_throughput` (all ops/sec) |
| `latency` | `avg_latency`, `avg_persist_latency` (ms) |
| `abort` | `agg_abort_rate`, `avg_per_core_abort_rate` (aborts/sec) |
| `memory` | `memory_delta`, `memory_delta_rate`, `logical_memory_delta`, `logical_memory_delta_rate` (MB / MB/sec) |
| `runtime` | `runtime_sec` |
| `txn_breakdown` | map of txn name → count (TPC-C: `Delivery`, `NewOrder`, `OrderStatus`, `Payment`, `StockLevel`; YCSB: `Read`; queue micro: empty) |

Missing fields are silently skipped — the parser does not crash on partial output, which lets the same parser handle TPC-C, YCSB, and the microbenchmarks.

### Example output

Sample `silo_default` result on a 56-core host:

```
agg_throughput:                 50510.9 ops/sec
avg_latency:                    0.0197062 ms
agg_abort_rate:                 0 aborts/sec
memory_delta:                   429.898 MB
runtime_sec:                    1.00018
txn_breakdown:
  Delivery=1980, NewOrder=22842, OrderStatus=1944, Payment=21677, StockLevel=2077
```

Sample `silo_ycsb_c` (4 threads, in-memory reads):

```
agg_throughput:                 1.16391e+07 ops/sec
avg_per_core_throughput:        2.90979e+06 ops/sec/core
avg_latency:                    0.000289753 ms
txn_breakdown: Read=23280692
```

## Build notes

- Pinned commit: `cc11ca1ea949ef266ee12a9b1c310392519d9e3b` (master tip from 2014).
- Build flags: `MODE=perf DEBUG=0 CHECK_INVARIANTS=0 USE_MALLOC_MODE=1`, C++ standard `-std=gnu++0x`.
- The masstree submodule's `.gitmodules` pins `git://` URLs that github no longer serves; the install script rewrites these to `https://`.
- The install script `sed`-patches Silo's `Makefile` to drop `-Werror` from the `MODE=perf` branch. Without this, modern GCC (≥ 11 on EL9) escalates warnings in `masstree/string_base.hh` (`-Wformat-truncation` on a `snprintf` size mismatch) and `btree.cc` (`-Wmaybe-uninitialized` on a `memcmp` of stack-constructed varkey) to fatal errors that the existing `-Wno-error=maybe-uninitialized` flag cannot fully suppress.
- The masstree `.so` is dynamically linked at runtime — do not delete `./silo_build/` if you want to keep running the binary. The cleanup script removes both the binary and the build tree.

## Representativeness

Silo represents the **in-memory OLTP database** workload class: many-core scaling, optimistic concurrency control, cache-conscious data structures, and the classic TPC-C / YCSB mix. It complements the existing DCPerf web/cache/analytics benchmarks by exercising the database tier directly without going through a SQL parser or wire protocol.

## Files in this package

| File | Purpose |
|---|---|
| `install_silo.sh` | Clones stephentu/silo at the pinned commit, builds `dbtest` with `MODE=perf USE_MALLOC_MODE=1`, installs the binary to `./benchmarks/silo/dbtest` |
| `cleanup_silo.sh` | Removes `./benchmarks/silo/` and the `silo_build/` tree |
| `run.sh` | Wrapper used as the benchmark `path:` — forwards all args to `dbtest`, rewrites `--num-threads 0` to `$(nproc)`, supports `SILO_NUMA_NODE` / `SILO_CPUS` / `SILO_BIN` |
| `README.md` | This file |
