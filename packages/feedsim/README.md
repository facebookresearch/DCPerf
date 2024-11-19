<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
-->
# Feedsim

FeedSim is a benchmark that represents the aggregation and ranking workloads in
recommendation systems. It searches for the maximum QPS that the system can
achieve while keeping p95 latency to be no greater than 500ms.

## Install feedsim

```
./benchpress_cli.py install feedsim_autoscale
```

## Run feedsim

### Recommended job - `feedsim_autoscale`

`feedsim_autoscale` is the version of Feedsim benchmark that can scale up on
systems with CPUs of very large core counts. It will spawn multiple Feedsim
workload instances at 100 cores per instance (rounded up), let them run in
parallel and aggregate final results when they finish. For example, if your
system has 256 cores, this job will spawn three Feedsim instances.

To run Feedsim benchmark, simply execute the following command

```
./benchpress_cli.py run feedsim_autoscale
```

This job also has the following optional parameters:
  - `num_instances`: manually specify the number of feedsim workload instances
  to run in parallel instead of automatically scaling with the CPU core count
  - `extra_args`: extra arguments you would like to pass to Feedsim's runner
  script. Available arguments can be viewed by running
  `./benchmarks/feedsim/run.sh -h`.

For example, if you would like to run two instances regardless the CPU core
count, you can run the following:

```
./benchpress_cli.py run feedsim_autoscale -i '{"num_instances": 2}'
```

If you want to run an experiment with fixed 100 QPS, you can run:

```
./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q 100"}'
```

This benchmark normally takes around 30 minutes to finish. It is recommended to
turn on CPU boost before running this benchmark, otherwise feedsim might not
converge and yield very low result.

When feedsim finds the optimal QPS that meets the SLA of <=500ms p95 latency, it
will execute a final run of 5 minutes using the optimal QPS. Therefore, if you
would like to collect system and microarch metrics for performance analysis,
collect those during the last 5 minutes of the benchmark. We expect the CPU
utilization during this period to be in the range of 60%~75%.

## Reporting and Measurement

After the feedsim benchmark finishing, benchpress will report the results in
JSON format like the following:

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

The result above is from an experiment on a system of 380-core CPU, where
`feedsim_autoscale` would spawn four instances. The result report will include
performance numbers of each individual instance (named `1` to `4`) as well as
the aggregated overall performance (named `overall`) in the `metrics` section.

`overall` performance is what we care about. Its metrics are calculated as follows:
  * `final_achieved_qps` - sum of `final_achieved_qps` of all individual instances
  * `final_requested_qps` - sum of `final_requested_qps` of all individual instances
  * `average_latency_msec` - average of final P95 latency of all individual instances

`final_achieved_qps` is the metric that measures the performance, measuring
the max QPS FeedSim could achieve with the contraint of p95 latency being
less than 500ms.

We expect all the individual instances have similar final achieved QPS numbers.
If you see one or more instances have significantly lower performance than the
others, it indicates the experiment is probably unsuccessful because the
low-performance instances may have got killed earlier than they should. In this
case, we suggest running this benchmark again.

Feedsim will generate detailed metrics reports at
`benchmark_metrics_<run_id>/feedsim_results_<1~N>.txt` (N is the workload instance ID
and each instanec will have its own CSV)
in CSV format like the following:

```
duration_secs,total_queries,requested_qps,achieved_qps,total_bytes_rx,total_bytes_tx,rx_MBps,tx_MBps,min_ms,avg_ms,50p_ms,90p_ms,95p_ms,99p_ms,99.9p_ms
120,5267,0.00,41.46,85637186,15927408,0.64,0.12,388.134,958.509,913.313,956.414,991.523,1812.133,12232.154
120,5571,0.00,43.86,90618516,16846704,0.68,0.13,388.134,910.627,918.002,960.395,1010.386,1050.379,1848.997
120,5040,39.97,39.67,82279002,15240960,0.62,0.11,388.134,457.068,446.739,524.929,535.555,569.859,593.868
120,2737,20.49,21.55,44727453,8276688,0.34,0.06,352.849,423.693,406.253,486.015,509.429,535.926,556.451
......

```

Besides, logs of individual instances will also be recorded at
`benchmark_metrics_<run_id>/feedsim-multi-inst-<1~N>.log` for debugging.

## Advanced Usage - Fixed-QPS experiment

Rather than let feedsim's runner script search for an optimal QPS that meets the
SLA, you can also choose to run feedsim at a fixed QPS and observe the latency.
To do so, simply run the following:

```sh
./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q <QPS>"}'
```
`-q` follows the desired QPS you would like **each instance** to drive,
and `-d` is an optional parameter stating the duration of the experiment
(default is 300s). Note that the total runtime could be longer because
feedsim needs to populate object graphs and perform a warmup. The default
warmup period is 120s, and can be changed by specifying the optional
parameter `-w`. Reducing warmup (and experiment) time can make the run
faster, but may affect accuracy and end up with lower QPS than the machine
can actually achieve.

For example, if you would like to drive 250 QPS in each feedsim instance
on a 380-core CPU system:

```
[root@<hostname> ~/external]# ./benchpress_cli.py run feedsim_autoscale -i '{"extra_args": "-q 250"}'
Will run 1 job(s)
......
Results Report:
{
  "benchmark_args": [
    "{num_instances}",
    "{extra_args}"
  ],
  "benchmark_desc": "Aggregator like workload. Latency sensitive. Finds maximum QPS that system can sustain while keeping 95th percentile latency <= 500 msecs. Automatically spawns multiple workload instances at 100 cores per instance (rounded
up).\n",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}",
    "copymove: {'is_move': True, 'after': ['benchmarks/feedsim/feedsim_results*.txt', 'benchmarks/feedsim/feedsim-multi-inst-*.log']}"
  ],
  "benchmark_name": "feedsim_autoscale",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU name>",
      "hostname": "<hostname>",
      "kernel_version": "5.19.0_xxxxx",
      "mem_total_kib": "2377504144 KiB",
      "num_logical_cpus": "380",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 9"
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
      "final_achieved_qps": 246.6,
      "final_latency_msec": 317.93,
      "final_requested_qps": 250.0
    },
    "2": {
      "final_achieved_qps": 246.71,
      "final_latency_msec": 312.45,
      "final_requested_qps": 250.0
    },
    "3": {
      "final_achieved_qps": 246.73,
      "final_latency_msec": 311.64,
      "final_requested_qps": 250.0
    },
    "4": {
      "final_achieved_qps": 246.4,
      "final_latency_msec": 316.28,
      "final_requested_qps": 250.0
    },
    "overall": {
      "average_latency_msec": 314,
      "final_achieved_qps": 986.44,
      "final_requested_qps": 1000.0
    },
    "successful_instances": 4,
    "target_latency_msec": "",
    "target_percentile": ""
  },
  "run_id": "80c1764b",
  "timestamp": 1703179008
}
Finished running "feedsim_autoscale": Aggregator like workload. Latency sensitive. Finds maximum QPS that system can sustain while keeping 95th percentile latency <= 500 msecs. Automatically spawns multiple workload instances at 100 cores per instance (rounded up).
 with uuid: 80c1764b
```

## Other extra args

Please refer to `./benchmarks/feedsim/run.sh -h` to see other available
parameters that you can supply to the `extra_args` parameter:

```
Usage: run.sh [-h] [-t <thrift_threads>] [-c <ranking_cpu_threads>]
                [-e <io_threads>]

    -h Display this help and exit
    -t Number of threads to use for thrift serving. Large dataset kept per thread. Default: 216
    -c Number of threads to use for fanout ranking work. Heavy CPU work. Default: 134
    -s Number of threads to use for task-based serialization cpu work. Default: 55
    -a When searching for the optimal QPS, automatically adjust the number of cliient driver threads by
       min(requested_qps / 4, 384 / 5) in each iteration (experimental feature).
    -q Number of QPS to request. If this is present, feedsim will run a fixed-QPS experiment instead of searching
       for a QPS that meets latency target. If multiple comma-separated values are specified, a fixed-QPS experiment
       will be run for each QPS value.
    -d Duration of each load testing experiment, in seconds. Default: 300
    -p Port to use by the LeafNodeRank server and the load drievrs. Default: 11222
    -o Result output file name. Default: "feedsim_results.txt"
```
