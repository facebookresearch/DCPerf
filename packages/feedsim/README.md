<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# FeedSim

FeedSim is a benchmark that represents the aggregation and ranking workloads
in production recommendation systems. It searches for the maximum QPS the
system can sustain while keeping p95 latency ≤ **700 ms**. Though it's possible
that on some platforms the CPU gets saturated before latency hitting the SLA.

The current recommended job is **`feedsim_dlrm`** (v2). It runs a
single-instance aggregator with real DLRM inference, out-of-process
`mock_services` RPC fanout over TLS + ZSTD, feature extraction and story
processors modules. Data used in the request flow is backed by the Silesia corpus
to ensure representative in compressibility. Because we replaced the fixed 200ms
sleep with hundreds of concurrent real RPC communication to the `mock_services`
module, the tail latency SLA is increased from 500ms to 700ms accordingly.

Because of the fundamental change in software architecture and threading model,
the legacy PageRank-based v1 job will not be supported here. If you would like to
run it, please check out the [main branch](https://github.com/facebookresearch/DCPerf/tree/main/packages/feedsim)
for Github users or the `v1` folder under the DCPerf root for internal users.

For the software architecture and request control/data flow of v2, see
[ARCHITECTURE_v2.md](ARCHITECTURE_v2.md).

## Recommended job — `feedsim_dlrm`

### Install

```
./benchpress_cli.py install feedsim_dlrm
```

The installer downloads and builds folly, fbthrift, wangle, fizz, mvfst,
LibTorch, the Silesia compression corpus, the DLRM TorchScript model, and
the FeedSim binaries. It runs ~30 minutes on a fresh host.

On memory-constrained hosts (e.g. machines with less than 1GB per core), we
recommend that you cap the build parallelism to avoid OOM, for example:

```
taskset -c 0-15 ./benchpress_cli.py install feedsim_dlrm
```

### Run

```
./benchpress_cli.py run feedsim_dlrm
```

Unlike FeedSim v1 which spawns a new FeedSim instance per 100 CPU cores,
`feedsim_dlrm` is pinned to **one FeedSim instance per host** because the
redesigned threading model in FeedSim v2 has overcome the scalability issue
on ultra-high-core-count CPUs and ARM CPUs.

The runner searches for the QPS that keeps 95th-percentile end-to-end
latency at or below **700 ms**. When it converges it runs a final 5-minute
steady-state pass at that QPS; collect telemetry (perf, mpstat, µarch
counters) during that final window. We expect the total wall-clock runtime
to be around 30 minutes.

Please make sure to turn CPU turbo-boost on before starting, or FeedSim may
fail to converge and report a low QPS.

On **high-performance ARM cores** the default driver depth (`depth=1`) often
caps the offered load below what the server can sustain — the run can finish
with CPU at only 80–90% and p95 latency well under the SLA, understating the
hardware's true capacity. In that case, rerun with `depth=2`:

```
./benchpress_cli.py run feedsim_dlrm -i '{"depth": 2}'
```

See [Driver depth](#driver-depth-fixing-cpulatency-under-utilization) for
details on when and how to tune this.

### Result report

After the run finishes, benchpress prints a JSON result. Example from a
176-core x86 host (256 GB RAM):

```json
{
  "benchmark_args": [
    "-n 1",
    "--depth=1"
  ],
  "benchmark_name": "feedsim_dlrm",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-name>",
      "hostname": "<server-hostname>",
      "kernel_version": "6.13.2-0_fbk8_0_g8695f611147d",
      "mem_total_kib": "263374740 KiB",
      "num_cpus_usable": 176,
      "num_logical_cpus": "176",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 9",
      "threads_per_core": "2"
    }
  ],
  "metadata": {
    "L1d cache": "2.8 MiB (88 instances)",
    "L1i cache": "2.8 MiB (88 instances)",
    "L2 cache": "88 MiB (88 instances)",
    "L3 cache": "176 MiB (11 instances)"
  },
  "metrics": {
    "1": {
      "final_achieved_qps": 278.84,
      "final_latency_msec": 644.04,
      "final_requested_qps": 281.03
    },
    "is_fixed_qps": 0,
    "max_qps": 278.84,
    "min_qps": 278.84,
    "overall": {
      "average_latency_msec": 644.04,
      "final_achieved_qps": 278.84,
      "final_requested_qps": 281.03
    },
    "score": 4.8919,
    "spawned_instances": "1",
    "successful_instances": 1,
    "target_latency_msec": "700",
    "target_percentile": "95p"
  },
  "run_id": "d8c6a288",
  "timestamp": 1782368295
}
```

`benchmark_args` lists only the arguments the job sets explicitly (`-n 1`
for single-instance, plus the driver `--depth`). The full calibrated recipe —
DLRM batch size, feature-extractor/story-processor settings, mock_services
RPC fanout, ZSTD fractions, etc. — is baked into the defaults of `run.sh`
and the `LeafNodeRank` command line, so it no longer appears here. To inspect
those defaults run `./benchmarks/feedsim/run.sh -h`, and to override any of
them use the [`feedsim_dlrm_custom`](#customized-runs--feedsim_dlrm_custom)
job.

Key fields are in `metrics.overall`:

- `final_achieved_qps` — the primary performance number. Max QPS the host
  sustained with p95 ≤ 700 ms.
- `average_latency_msec` — the observed p95 at that QPS. The latency number
  can be around or below `target_latency_msec` (700) for a converged run.
  This field is called "average" because it's the average of p95 latency
  values observed from all benchmark instances (which will be discussed later).
  It itself still indicates the tail p95 latency.
- `final_requested_qps` — what the driver asked for. Should be within a few
  percent of `final_achieved_qps` on a converged run.

`score` is `final_achieved_qps` divided by the QPS number achieved on an
older reference system to reflect the platform's relative speedup.

`is_fixed_qps` is `0` for the default search mode. It is `1` when you pass
`-q` (see [Fixed-QPS experiments](#fixed-qps-experiments) below), and the
`overall` block then reports observed p95 at the fixed load rather than a
saturation point.

Per-instance CSVs live at
`benchmark_metrics_<run_id>/feedsim_results_<1>.txt`; instance logs at
`benchmark_metrics_<run_id>/feedsim-multi-inst-<1>.log`; the runtime
breakdown at `benchmark_metrics_<run_id>/breakdown.csv`.

## DCPerf Mini: `feedsim_dlrm_mini`

`thrift_threads` is the knob that controls peak memory usage. Each thrift serving
thread keeps its own copy of the dataset, at roughly **0.33GB per thread**.
It defaults to `0`, meaning `min(nproc, 216)`, so the footprint scales with
the machine (or with the cpuset the job runs in).

Please adjust it as following:

```
./benchpress_cli.py run feedsim_dlrm_mini -i '{"thrift_threads": "8"}'
```

| `thrift_threads` | Peak memory (measured, Grace) |
|------------------|-------------------------------|
| 6                | ~2.7GB                        |
| 8                | ~3.3GB                        |
| 16               | ~5.4GB                        |

`thrift_threads=8` is the recommended value to stay under a 4GB budget.

## Advanced usage

### Fixed-QPS experiments

Instead of searching for the SLA-bound QPS, drive a fixed QPS and observe
latency:

```
./benchpress_cli.py run feedsim_dlrm -i '{"extra_args": "-q <QPS>"}'
```

`-q` accepts either a single value or a comma-separated list of QPS values;
in the latter case, FeedSim runs one experiment per value in sequence.
`-d <seconds>` sets the per-experiment duration (default 300 s). Warmup
adds ~120 s to each experiment, and object-graph population adds ~1–2 min
on top; a single fixed-QPS point takes ~7 min end-to-end.

Example — sweep low-load points to inspect the p95 cliff at cold-channel:

```
./benchpress_cli.py run feedsim_dlrm -i '{"extra_args": "-q 5,20,50,100,150"}'
```

### Multi-instance (max throughput)

If you encounter insufficient scalability / low CPU utilization problem on some
new hardware platform in the future, or you want to test multi NUMA-node systems,
you can try the multi-instance option in Feedsim V2. There are two ways to launch
multiple instances:

1. Specify `num_instances` parameter when running the `feedsim_dlrm` job. For example,
`./benchpress_cli.py run feedsim_dlrm -i '{"num_instances": 2}'` will launch 2 sets of
feedsim server, driver and mock_services instances.

2. Use the `feedsim_autoscale_dlrm` job. This autoscale job will spawn `ceil(nproc / 100)`
FeedSim instances, each pinned to its own CPU range via `taskset`, plus one driver
and one `mock_services` process per instance (also `taskset`-isolated). For example:
```
./benchpress_cli.py run feedsim_autoscale_dlrm
```

In multi-instance mode, the overall QPS is the sum across all instances. and the
average latency will be the average of p95 latency values observed across all
instances.

### Driver depth (fixing CPU/latency under-utilization)

The `depth` parameter sets the driver's pipeline depth — the maximum number of
outstanding (in-flight) requests per driver connection. The driver's total
offered concurrency is `driver_threads × connections × depth`, so with the
default `depth=1` the driver can cap the achievable load below what the server
can actually handle.

**Increase `depth` beyond 1 when the final benchmarking phase saturates neither
CPU nor latency** — i.e. the final achieved p95 latency is well below the SLA
limit (`sla_p95_ms`, default 700 ms) *and* the CPU utilization during the final
5-minute benchmarking phase is less than ~90%. In that situation the reported QPS
is limited by driver concurrency rather than by the server, so it understates the
hardware's true capacity. Raising `depth` (start with `2`) lets the driver offer
more concurrent load until the server becomes the bottleneck — either CPU-bound
(~100% utilization) or latency-bound (p95 ≈ SLA). **This is likely necessary on
high-performance ARM cores**, which can otherwise sit at 80–90% CPU with p95 far
below the SLA at `depth=1`.

```
# Force driver depth 2
./benchpress_cli.py run feedsim_dlrm -i '{"depth": 2}'
```

There is also an **adaptive depth** mechanism (on by default) that raises the
depth automatically during the peak-finding stage until the server saturates
(system CPU ≥ 95% or p95 ≥ SLA). It catches *severe* under-utilization early, but
because it evaluates saturation on the high-load peak/search probes rather than
on the final SLA-converged operating point, it **may not catch all
under-utilization cases**. If you still observe under-utilization in the final
result (low CPU + p95 well under SLA), increase `depth` manually as above. When
adaptive depth is on, a manually-set `depth` acts as the starting floor the
adaptive search raises from; to pin an exact fixed depth, also set the
`FEEDSIM_ADAPTIVE_DEPTH_MAX=0` environment variable to disable adaptive search.

### Customized runs — `feedsim_dlrm_custom`

`feedsim_dlrm` and `feedsim_autoscale_dlrm` intentionally expose only a couple
of knobs (`num_instances`, `depth`); the rest of the calibrated recipe is baked
into the `run.sh` / `LeafNodeRank` defaults so that standard runs are directly
comparable across platforms. For parameter sweeps and one-off experiments,
`feedsim_dlrm_custom` runs the *same* workload with identical defaults but
surfaces every calibrated parameter as an explicit var you can override with
`-i` — no need to edit `run.sh`:

```
./benchpress_cli.py run feedsim_dlrm_custom -i '{"dlrm_batch_size": "64", "server_zstd": "0"}'
```

The overridable vars and their (calibrated) defaults are listed in
[Other parameters](#other-parameters) below. Use `feedsim_dlrm` or
`feedsim_autoscale_dlrm` for the standard calibrated runs.

### Other parameters

This section lists the calibrated parameters of the DLRM workload. They are
carefully tuned to make the benchmark's performance characteristics closely
match the ranking & aggregation systems in production, so we **do not**
recommend you change them just for better performance — change them only for
research purposes.

These parameters are **baked into the `run.sh` / `LeafNodeRank` defaults**, so
`feedsim_dlrm` and `feedsim_autoscale_dlrm` no longer accept them via `-i`
(those jobs expose only `num_instances`, `depth`, and `extra_args`). To
override any of them, use the [`feedsim_dlrm_custom`](#customized-runs--feedsim_dlrm_custom)
job, where each is exposed as a var:

| Var | Purpose | Default |
|---|---|---|
| `num_instances` | Number of FeedSim instances to run in parallel. `1` for `feedsim_dlrm`; `feedsim_autoscale_dlrm` autoscales to `ceil(nproc / 100)`. | `1` |
| `sla_p95_ms` | SLA target in ms. The runner searches for the highest QPS keeping p95 ≤ this. | `700` |
| `depth` | Driver pipeline depth (max outstanding requests per connection; total in-flight = `driver_threads × connections × depth`). Raise (e.g. `2`) when the final phase saturates neither CPU nor latency — often needed on high-perf ARM. See [Driver depth](#driver-depth-fixing-cpulatency-under-utilization). | `1` |
| `async_io` | Async (non-blocking) I/O mode; eliminates thread starvation on high-core CPUs. Set `0` to disable. | `1` |
| `io_dist` | I/O latency distribution: `fixed`, `exponential`, or `lognormal`. | `fixed` |
| `io_mean` | Mean I/O latency in ms. | `200` |
| `dlrm_model` | Path to the TorchScript DLRM model (`.pt`). | `models/dlrm_small.pt` |
| `dlrm_batch_size` | Batch size for DLRM inference. | `32` |
| `dlrm_threads` | LibTorch intra-op thread count. | `1` |
| `dlrm_inferences` | DLRM inference calls per request. | `1` |
| `feature_extractors` | Enable the feature-extraction pipeline before ranking inference. Set `0` to disable. | `1` |
| `feature_complexity` | Complexity level (1–10) of the feature-extractor pipeline. Higher = more work per story. | `8` |
| `num_stories` | Stories per request (server-side feature extraction). | `400` |
| `extractors_per_story` | Feature extractors randomly selected per story. | `240` |
| `story_processors_per_story` | Story-processor pipeline passes per story (0 disables). Mirrors prod scoring + filter + blend + serdes + topK. | `2` |
| `stories_per_processor_pass` | MockStories per story-processor pass. | `150` |
| `stories_per_request` | Silesia snippet count per request. | `10` |
| `mock_tls` | Enable TLS on outbound `MockServicesClient` channels. | `1` |
| `mock_zstd_frac` | Fraction of `MockServicesClient` channels with ZSTD enabled. | `0.9` |
| `mock_keepalive_interval_ms` | Keepalive ping interval per channel (0 disables). Defeats the cold-channel p95 cliff at low QPS. | `200` |
| `rpc_fanout_scale` | Scale factor applied to per-session fanout counts. | `0.10` |
| `server_zstd` | Enable ZSTD compression on server-side response payloads. | `1` |
| `silesia_dir` | Silesia corpus directory (resolved under the feedsim root). | `silesia` |
| `extra_args` | Escape hatch — appended to the runner CLI verbatim. | `""` |

Note:
1. `io_dist` and `io_mean` will not actually be used in Feedsim V2, the actual
RPC latency distribution is controlled by `rpc_dist.json`.
2. Meaning of `rpc_fanout_scale`: when it's 1, each request will fan out over 2000
RPC requests to the mock_services (which is the average number of outbound RPC calls
the ranking server will do for each incoming request). That will be too heavy for this
benchmark, so we need to set this to a lower number.

Runner-level parameters (can be passed via `extra_args` parameter):

| Flag | Purpose | Default |
|---|---|---|
| `-q <QPS[,QPS...]>` | Run at fixed QPS instead of searching. | search mode |
| `-d <sec>` | Per-experiment duration. | `300` |
| `-p <port>` | `LeafNodeRank` listen port. | `11222` |
| `-N` | No-retry mode. Skip sleep and PID checking in load test startup. | off |
| `-D <sec>` | Drain time after each experiment. | `5` |
| `-R / -C` | Seeds for the LeafNodeRank / PointerChase RNGs. | time-based |
| `-o <file>` | Result output filename. | `feedsim_results.txt` |

For the full list of runner flags, see
`./benchmarks/feedsim/run.sh -h`.

## Legacy — `feedsim_autoscale` (v1)

Again, due to the fundamental change in software architecture and threading model,
the legacy PageRank-based v1 job has become inaccurate and will not be supported here. If you would like to
run it, please check out the [main branch](https://github.com/facebookresearch/DCPerf/tree/main/packages/feedsim)
for Github users or the `v1` folder under the DCPerf root for internal users.
