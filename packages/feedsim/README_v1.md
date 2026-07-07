<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# FeedSim v1 (legacy) — `feedsim_autoscale`

> **Deprecated.** New experiments should use `feedsim_dlrm` (v2). See
> [README.md](README.md) for the current recommended job. This document is
> preserved for reference and back-compat with older result sets that used
> the 500ms SLA and PageRank workload.

FeedSim v1 is the original aggregation/ranking benchmark shipped with DCPerf.
It searches for the maximum QPS that the system can sustain while keeping
95th-percentile latency at or below **500ms**. The workload is a synthetic
PageRank-style ranking loop with a `folly::futures::sleep`-modeled I/O phase;
outbound RPC fanout, TLS, and DLRM inference are not part of v1.

## Install

```
./benchpress_cli.py install feedsim_autoscale
```

## Run

### Recommended v1 job — `feedsim_autoscale`

`feedsim_autoscale` scales up on systems with very large core counts. It
spawns multiple FeedSim workload instances at 100 cores per instance
(rounded up), runs them in parallel, and aggregates results. For example,
on a 256-core system this job spawns three FeedSim instances.

```
./benchpress_cli.py run feedsim_autoscale
```

Optional parameters:

- `num_instances`: manually specify how many workload instances to run in
  parallel instead of autoscaling with the core count.
- `extra_args`: extra arguments passed to FeedSim's runner script. Available
  arguments can be viewed with `./benchmarks/feedsim/run.sh -h`.

Example — force two instances regardless of core count:

```
./benchpress_cli.py run feedsim_autoscale -i '{"num_instances": 2}'
```

Example — fixed 100 QPS per instance:

```
./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q 100"}'
```

The benchmark takes ~30 minutes. Turn on CPU boost before running; otherwise
FeedSim may not converge and will yield a very low result. When FeedSim finds
the optimal QPS that meets the ≤500ms p95 SLA, it runs a final 5-minute
steady-state pass at that QPS. Collect system and microarchitecture metrics
during this final window. Expected CPU utilization: 60–75%.

### ARM

```
./benchpress_cli.py run feedsim_autoscale_arm
```

### FeedSim Mini

`feedsim_autoscale_mini` runs the same workload with minimal settings to keep
execution time to a few seconds, for quick testing. A `breakdown.csv` is
generated in the results so you can filter metrics to the actual benchmark
window.

```
./benchpress_cli.py run feedsim_autoscale_mini
```

## Reporting

After the benchmark finishes, benchpress reports results in JSON:

```
{
  "benchmark_args": [],
  "benchmark_desc": "Aggregator like workload. Latency sensitive. Finds maximum QPS that system can sustain while keeping 95th percentile latency <= 500 msecs.\n",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}",
    "copymove: {'is_move': True, 'after': ['benchmarks/feedsim/feedsim_results*.txt', 'benchmarks/feedsim/feedsim-multi-inst-*.log']}"
  ],
  "benchmark_name": "feedsim_autoscale",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-name>",
      "hostname": "<server-hostname>",
      "kernel_version": "5.19.0-0_xxxx",
      "mem_total_kib": "2377231352 KiB",
      "num_logical_cpus": "380",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 8"
    }
  ],
  "metadata": {
    "L1d cache": "6 MiB (192 instances)",
    "L1i cache": "6 MiB (192 instances)",
    "L2 cache": "192 MiB (192 instances)",
    "L3 cache": "768 MiB (24 instances)"
  },
  "metrics": {
    "1": {
      "final_achieved_qps": 248.38,
      "final_latency_msec": 310.9,
      "final_requested_qps": 251.82
    },
    "2": {
      "final_achieved_qps": 248.98,
      "final_latency_msec": 308.31,
      "final_requested_qps": 252.61
    },
    "3": {
      "final_achieved_qps": 249.73,
      "final_latency_msec": 305.82,
      "final_requested_qps": 252.98
    },
    "4": {
      "final_achieved_qps": 248.98,
      "final_latency_msec": 305.22,
      "final_requested_qps": 251.99
    },
    "overall": {
      "average_latency_msec": 307.56,
      "final_achieved_qps": 996.07,
      "final_requested_qps": 1009.4
    },
    "spawned_instances": "4",
    "successful_instances": 4,
    "target_latency_msec": "500",
    "target_percentile": "95p",
    "score": 17.4736842105
  },
  "run_id": "2ef4dfad",
  "timestamp": 1702590806
}
```

The result above is from a 380-core CPU, where `feedsim_autoscale` spawned
four instances (`1` through `4`). `overall` is the aggregate we compare:

- `final_achieved_qps` — sum of per-instance `final_achieved_qps`
- `final_requested_qps` — sum of per-instance `final_requested_qps`
- `average_latency_msec` — average of per-instance final p95 latency

`final_achieved_qps` is the primary performance metric: max QPS with p95
latency ≤500ms. Individual instances should all show similar QPS; if one is
significantly lower than the others, the run likely didn't converge cleanly
(a low-performance instance was probably killed early). Re-run in that case.

Detailed per-instance metrics live at
`benchmark_metrics_<run_id>/feedsim_results_<1..N>.txt` in CSV form:

```
duration_secs,total_queries,requested_qps,achieved_qps,total_bytes_rx,total_bytes_tx,rx_MBps,tx_MBps,min_ms,avg_ms,50p_ms,90p_ms,95p_ms,99p_ms,99.9p_ms
120,5267,0.00,41.46,85637186,15927408,0.64,0.12,388.134,958.509,913.313,956.414,991.523,1812.133,12232.154
120,5571,0.00,43.86,90618516,16846704,0.68,0.13,388.134,910.627,918.002,960.395,1010.386,1050.379,1848.997
120,5040,39.97,39.67,82279002,15240960,0.62,0.11,388.134,457.068,446.739,524.929,535.555,569.859,593.868
120,2737,20.49,21.55,44727453,8276688,0.34,0.06,352.849,423.693,406.253,486.015,509.429,535.926,556.451
...
```

Per-instance logs are at `benchmark_metrics_<run_id>/feedsim-multi-inst-<1..N>.log`.

## Advanced usage — fixed-QPS experiment

Instead of letting the runner search for the optimal QPS, run FeedSim at a
fixed QPS and observe latency:

```sh
./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q <QPS>"}'
```

`-q` is the QPS driven **per instance**; `-d` is the experiment duration
(default 300s). Total wall-clock is longer because FeedSim also populates
object graphs and warms up. Default warmup is 120s (change with `-w`).
Shorter warmups reduce accuracy and can under-report the achievable QPS.

Example — 250 QPS per instance on a 380-core system:

```
[root@<hostname> ~/external]# ./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q 250"}'
...
"metrics": {
  "1": {"final_achieved_qps": 246.6,  "final_latency_msec": 317.93, "final_requested_qps": 250.0},
  "2": {"final_achieved_qps": 246.71, "final_latency_msec": 312.45, "final_requested_qps": 250.0},
  "3": {"final_achieved_qps": 246.73, "final_latency_msec": 311.64, "final_requested_qps": 250.0},
  "4": {"final_achieved_qps": 246.4,  "final_latency_msec": 316.28, "final_requested_qps": 250.0},
  "overall": {"average_latency_msec": 314, "final_achieved_qps": 986.44, "final_requested_qps": 1000.0}
}
```

## Other extra args

See `./benchmarks/feedsim/run.sh -h`. The v1-relevant subset:

```
Usage: run.sh [OPTION]...

    -t Number of threads to use for thrift serving. Large dataset kept per thread. Default: 216
    -c Number of threads to use for fanout ranking work. Heavy CPU work. Default: 134
    -s Number of threads to use for task-based serialization cpu work. Default: 55
    -a Auto-adjust client driver threads by min(requested_qps / 4, 384 / 5) per search iteration.
    -q Fixed QPS. If multiple comma-separated values, one experiment per value.
    -d Experiment duration in seconds. Default: 300
    -p Port for LeafNodeRank + drivers. Default: 11222
    -o Result output filename. Default: "feedsim_results.txt"
```

## Migrating to v2

The v2 recommended job is `feedsim_dlrm` — DLRM inference, out-of-process
`mock_services` fanout with TLS + ZSTD, session-mode driver, and a 700ms p95
SLA that mirrors production `multifeed_aggregator`. See
[README.md](README.md) for install/run instructions and
[ARCHITECTURE_v2.md](ARCHITECTURE_v2.md) for the software architecture and
control/data flow.
