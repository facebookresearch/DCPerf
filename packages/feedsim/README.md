# feedsim

FeedSim is a benchmark that represents Feed Aggregator workload. It searches for
the maximum QPS that the system can achieve while keeping p95 latency to be no
greater than 500ms.

## Install feedsim

```
./benchpress_cli.py install feedsim_default
```

## Run feedsim

```
./benchpress_cli.py run feedsim_default
```

This benchmark normally takes around 30 minutes to finish. It is recommended to
turn on CPU boost before running this benchmark, otherwise feedsim might not
converge and yield very low result.

When feedsim finds the optimal QPS that meets the SLA of <=500ms p95 latency, it
will execute a final run of 5 minutes using the optimal QPS. Therefore, if you
would like to collect system and microarch metrics for performance analysis,
collect those during the last 5 minutes of the benchmark.

## Reporting

After the feedsim benchmark finishing, benchpress will report the results in
JSON format:

```
{
  "benchmark_args": [],
  "benchmark_desc": "",
  "benchmark_hooks": [
    "cpu-mpstat: {'args': ['-u', '1']}"
  ],
  "benchmark_name": "feedsim_default",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "Intel(R) Xeon(R) Platinum 8321HC CPU @ 1.40GHz",
      "hostname": "<hostname>",
      "kernel_version": "5.6.13-...",
      "mem_total_kib": "65385308 KiB",
      "num_logical_cpus": "52",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 8"
    }
  ],
  "metadata": {
    "L1d cache": "32K",
    "L1i cache": "32K",
    "L2 cache": "1024K",
    "L3 cache": "36608K"
  },
  "metrics": {
    "final_achieved_qps": 25.31,
    "final_latency_msec": 1009.95,
    "final_requested_qps": 0.08,
    "target_latency_msec": 500.0,
    "target_percentile": "95p"
  },
  "run_id": "eab7e5dd",
  "timestamp": 1650388812
}
```

Feedsim will also generate a detailed metrics report at
`benchmark_metrics_<run_id>/feedsim_results.txt` in CSV format like the following:

```
duration_secs,total_queries,requested_qps,achieved_qps,total_bytes_rx,total_bytes_tx,rx_MBps,tx_MBps,min_ms,avg_ms,50p_ms,90p_ms,95p_ms,99p_ms,99.9p_ms
120,5267,0.00,41.46,85637186,15927408,0.64,0.12,388.134,958.509,913.313,956.414,991.523,1812.133,12232.154
120,5571,0.00,43.86,90618516,16846704,0.68,0.13,388.134,910.627,918.002,960.395,1010.386,1050.379,1848.997
120,5040,39.97,39.67,82279002,15240960,0.62,0.11,388.134,457.068,446.739,524.929,535.555,569.859,593.868
120,2737,20.49,21.55,44727453,8276688,0.34,0.06,352.849,423.693,406.253,486.015,509.429,535.926,556.451
......

```

## Advanced Usage - Fixed-QPS experiment

Rather than let feedsim's runner script search for an optimal QPS that meets the
SLA, you can also choose to run feedsim at a fixed QPS and observe the latency.
To do so, simply run the following:

```sh
./benchmarks/feedsim/run.sh -q <QPS> [-d <duration-secs>]
```
`-q` follows the desired QPS you would like to drive, and `-d` is an optional
parameter stating the duration of the experiment (default is 300s). Note that
the total runtime could be longer because feedsim needs to warmup.

For example:

```
(base) [root@hostname ~/external]# ./benchmarks/feedsim/run.sh -q 100
LeafServer listening on 0.0.0.0:11222
Monitor Server listening on port 8888
Generate Time:       2.09485
Generate Time:       2.11056
Generate Time:       2.11753
Generate Time:       2.11962
Generate Time:       2.13266
...
...
Build Time:          24.12016
Build Time:          24.10939
Build Time:          24.29443
Build Time:          24.11943
Build Time:          24.17573
Running an experiment with QPS fixed at 100 and returns 95p latency
warmup qps = 217.30, latency = 620.60
peak qps = 226.50, latency = 616.42
requested_qps = 100.00, measured_qps = 105.52, latency = 394.26
```
