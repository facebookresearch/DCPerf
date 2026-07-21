<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# GAPBS (multi-instance)

GAPBS is the [GAP Benchmark Suite](https://github.com/sbeamer/gapbs) — a set of
graph-analytics kernels (`bc`, `bfs`, `cc`, `pr`, `sssp`, `tc`). This package
adds `gapbs_multi`, a memory-focused, multi-instance driver: it launches N
core-pinned gapbs instances of one kernel in parallel against a **pre-built**
graph, each instance in its own cgroup with per-instance memory limits, and
aggregates the per-instance `Average Time` (mean / min / max).

It is intended for large-memory / tiered-memory (e.g. CXL) characterization,
where several graph-analytics instances run side by side under controlled cpu
and memory budgets.

> **Note:** `gapbs_multi` lives in the `mem` benchmark suite, so every command
> must pass `-b mem`.

## Install

```
./benchpress_cli.py -b mem install gapbs_multi
```

This builds the gapbs kernels + `converter` and copies `run-gapbs-multi.sh` and
`generate_graph.sh` into `./benchmarks/gapbs/`.

## Generate the graph

`gapbs_multi` never generates graphs at run time — build one once with the
`generate_graph.sh` helper. The default reproduces `kronecker30_k33.sg`
(a Kronecker graph with 2^30 vertices and average degree 33, ~350 GB):

```
./benchmarks/gapbs/generate_graph.sh -g 30 -k 33
```

The script **aborts unless the output filesystem has at least 500 GB free**
(override with `--min-free-gb`). Useful options:

  - `-g <scale>` / `-k <degree>`: graph size (2^scale vertices, avg degree)
  - `-b <name|file>`: output base name or path (default `kronecker<scale>_k<degree>`)
  - `-d <dir>`: output directory (default `./benchmarks/gapbs`)
  - `--weighted`: build a weighted graph (`.wsg`, required for `sssp`)
  - `--undirected`: symmetrize + `U` suffix (`.U.sg`, required for `tc`)
  - `--min-free-gb <n>`: change the disk-space guard (default 500)
  - `--force`: regenerate even if the target already exists

See `./benchmarks/gapbs/generate_graph.sh -h` for the full list.

## Run

### Recommended job - `gapbs_multi`

```
./benchpress_cli.py -b mem run gapbs_multi
```

Each instance is launched as a transient `systemd-run --scope`, i.e. its **own
cgroup**, with `AllowedCPUs` (reserved cores) and `MemoryLow` / `MemoryHigh` /
`MemoryMax` limits. The kernel then runs on `threads_per_instance` cores (exact
OpenMP thread count).

> **Root required:** per-instance memory limits need root (to create the scope).
> Without root, the runner warns and falls back to `taskset` pinning **without**
> memory limits.

This job has the following optional parameters (override with `-i '{...}'`):

  - `num_instances`: number of parallel instances (default `3`)
  - `bench`: kernel to run — `bc` | `bfs` | `cc` | `pr` | `sssp` | `tc` (default `bc`)
  - `graph_file`: graph base name (no extension) or full `.sg`/`.wsg` path,
    resolved per kernel: `bc/bfs/cc/pr` → `.sg`, `sssp` → `.wsg`, `tc` → `U.sg`
    (default `kronecker30_k33`)
  - `trials`: timed trials, passed to gapbs `-n` (default `2`)
  - `core_start` / `cores_per_instance` / `core_stride`: physical-core layout
    (defaults `0` / `32` / `32`). Counts are **physical cores**; SMT siblings are
    auto-added to each instance's reserved cpuset.
  - `threads_per_instance`: active OpenMP thread count per instance (default `16`)
  - `mem_low` / `mem_high` / `mem_max`: per-instance memory limits, accept human
    sizes (e.g. `290G`) or `max` = unlimited (defaults `290G` / `300G` / `310G`)
  - `extra_args`: passed verbatim to the runner (see below)

Available `extra_args` (also visible via `./benchmarks/gapbs/run-gapbs-multi.sh -h`):

  - `--ccx`: bind instance *i* to auto-detected CCX[*i*] (sysfs L3 domain)
    instead of the explicit stride layout
  - `--mem-nodes 0,1`: set `cpuset.mems` / `AllowedMemoryNodes` (e.g. DRAM+CXL)
  - `--no-cgroup`: disable systemd scopes and memory limits (taskset only)

### Examples

Run **4 instances, each in its own cgroup** (needs root; default layout reserves
physical cores `0-31 / 32-63 / 64-95 / 96-127`, so ≥128 physical cores):

```
sudo ./benchpress_cli.py -b mem run gapbs_multi -i '{"num_instances": 4}'
```

Run 4 instances on a smaller host (16 reserved cores / 8 threads each):

```
sudo ./benchpress_cli.py -b mem \
  -o 'gapbs_multi: -n 4 -b bc -f kronecker30_k33 -t 2 -c 16 --core-stride 16 -T 8 --mem-low 290G --mem-high 300G --mem-max 310G' \
  run gapbs_multi
```

Bind each instance to a detected CCX and place memory on DRAM+CXL (nodes 0,1):

```
sudo ./benchpress_cli.py -b mem run gapbs_multi \
  -i '{"num_instances": 4, "extra_args": "--ccx --mem-nodes 0,1"}'
```

Run the `bfs` kernel with 30 trials:

```
sudo ./benchpress_cli.py -b mem run gapbs_multi -i '{"bench": "bfs", "trials": 30}'
```

## Core layout

For the default (stride) layout, instance *i* reserves the physical cores

```
[core_start + i*core_stride, core_start + i*core_stride + cores_per_instance - 1]
```

plus each of those cores' SMT siblings — this becomes the cgroup `AllowedCPUs`.
The kernel actually runs on only `threads_per_instance` physical cores (one
logical CPU per core), enforced with `taskset` + `OMP_NUM_THREADS`, matching the
"reserved cores vs. active threads" split used for graph-analytics runs.

With `--ccx`, the reserved cpuset is instead a full auto-detected CCX (the L3
`shared_cpu_list` from sysfs); instance *i* maps to CCX[*i*], and the run fails
if there are fewer CCXs than instances.

## Reporting and Measurement

`gapbs_multi` prints an aggregate block to stdout and reports it in JSON. The
headline metric is `average_time` — the **mean** of the per-instance
`Average Time` values (the runner also prints `average_time_min` /
`average_time_max` / `per_instance_average_time` for reference):

```
=== gapbs_multi aggregate (bc, 4 instances) ===
per_instance_average_time: 3.10 3.22 3.05 3.18
average_time_mean=3.137500
average_time_min=3.050000
average_time_max=3.220000
spawned_instances=4
successful_instances=4
Average Time: 3.137500
```

Result locations:

  - Parsed metrics JSON: `benchmark_metrics_<run_id>/gapbs_multi_metrics_<timestamp>_iter_*.json`
    (plus `gapbs_multi_system_specs_<timestamp>.json`), under the benchpress root
    or the directory given by `--artifacts-dir`
  - Run history: `./results` (or the directory given by `-r`)
  - Per-instance raw kernel logs (on the host): `/tmp/gapbs_multi_<bench>/benchlog_inst*.txt`

To collect system / microarchitecture metrics during the run, add hooks with
`-k`, e.g. `-k cpu-mpstat perf`. These are host-wide, time-window collectors and
cover all instances for the full duration of the run.

## Runner help

```
./benchmarks/gapbs/run-gapbs-multi.sh -h
```
