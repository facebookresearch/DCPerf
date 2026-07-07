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

For the legacy PageRank-based v1 job (`feedsim_autoscale`, 500 ms SLA), see
[README_v1.md](README_v1.md).

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

### Result report

After the run finishes, benchpress prints a JSON result. Example from a
AMD Zen4 host (176 logical cores, 256 GB RAM):

```json
{
  "benchmark_args": [
    "-n 1",
    "--async-io",
    "--io-dist=fixed",
    "--io-mean=200",
    "--workload=dlrm",
    "--dlrm-model=models/dlrm_small.pt",
    "--dlrm-batch-size=64",
    "--dlrm-threads=1",
    "--dlrm-inferences=1",
    "--client-side-features=0",
    "--client-batch-size=256",
    "--client-inferences=64",
    "--client-feature-seed=42",
    "--client-num-dense=13",
    "--client-num-sparse=26",
    "--feature-extractors",
    "--feature-complexity=5",
    "--num-stories=400",
    "--extractors-per-story=280",
    "--story-processors-per-story=2",
    "--stories-per-processor-pass=100",
    "--silesia-dir=silesia",
    "--stories-per-request=10",
    "--mock-tls=1",
    "--mock-zstd-frac=0.75",
    "--mock-keepalive-interval-ms=200",
    "--rpc-fanout-scale=0.05",
    "--server-zstd=0",
    "--sla-p95-ms=700"
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

### Other parameters

This section lists additional parameters in `feedsim_dlrm` benchmark. These parameters
are carefully tuned to make the benchmark's performance characteristics closely match
the ranking & aggregation systems in production, so we **do not** recommend you change
them just for better performance, you should change them only for research purpose.

Job-level parameters (can be passed via `-i` flag in Benchpress CLI):

| Var | Purpose | Default (`feedsim_dlrm`) |
|---|---|---|
| `num_instances` | Number of FeedSim instances to run in parallel. Defaults to 1 in `feedsim_dlrm`; set to -1 to autoscale for `feedsim_autoscale_dlrm`. | `1` |
| `sla_p95_ms` | SLA target in ms. The runner searches for the highest QPS keeping p95 ≤ this. | `700` |
| `io_dist` | I/O latency distribution: `fixed`, `exponential`, or `lognormal`. | `fixed` |
| `io_mean` | Mean I/O latency in ms. | `200` |
| `workload` | Ranking workload: `pagerank` or `dlrm`. `dlrm` is v2. | `dlrm` |
| `dlrm_model` | Path to the TorchScript DLRM model (`.pt`). | `models/dlrm_small.pt` |
| `dlrm_batch_size` | Batch size for DLRM inference. | `64` |
| `dlrm_threads` | LibTorch intra-op thread count. | `1` |
| `dlrm_inferences` | DLRM inference calls per request. | `1` |
| `feature_complexity` | Complexity level (1–10) of the feature-extractor pipeline. Higher = more work per story. | `5` |
| `num_stories` | Stories per request (server-side feature extraction). | `400` |
| `extractors_per_story` | Feature extractors randomly selected per story. | `280` |
| `story_processors_per_story` | Story-processor pipeline passes per story (0 disables). Mirrors prod scoring + filter + blend + serdes + topK. | `2` |
| `stories_per_processor_pass` | MockStories per story-processor pass. | `100` |
| `stories_per_request` | Silesia snippet count per request. | `10` |
| `mock_tls` | Enable TLS on outbound `MockServicesClient` channels. | `1` |
| `mock_zstd_frac` | Fraction of `MockServicesClient` channels with ZSTD enabled. | `0.75` |
| `mock_keepalive_interval_ms` | Keepalive ping interval per channel (0 disables). Defeats the cold-channel p95 cliff at low QPS. | `200` |
| `rpc_fanout_scale` | Scale factor applied to per-session fanout counts. | `0.05` |
| `server_zstd` | Enable ZSTD compression on server-side response payloads. | `0` |
| `client_side_features` | Enable client-side DLRM feature generation. | `0` |
| `silesia_dir` | Silesia corpus directory (auto-detected in benchmarks/feedsim/silesia/). | `silesia` |
| `extra_args` | Escape hatch — appended to the runner CLI verbatim. | `""` |

Note:
1. `io_dist` and `io_mean` will not actually be used in Feedsim V2, the actual
RPC latency distribution is controlled by `rpc_dist.json`.
2. Changing `workload` will not only change the type of ranking workload, but
also change the entire request workflow. `dlrm` will use the new FeedSim v2
path, whereas `pagerank` will use the legacy Feedsim V1 path.
3. Meaning of `rpc_fanout_scale`: when it's 1, each request will fan out over 2000
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
| `-R / -P / -C` | Seeds for LeafNodeRank / PageRank / PointerChase RNGs. | time-based |
| `-o <file>` | Result output filename. | `feedsim_results.txt` |

For the full list of runner flags, see
`./benchmarks/feedsim/run.sh -h`.

## Legacy — `feedsim_autoscale` (v1)

The v1 PageRank-based job and its 500ms SLA are documented in
[README_v1.md](README_v1.md).
