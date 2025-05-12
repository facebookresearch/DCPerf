# PerfPub

PerfPub is a convenient tool that analyzes and summarizes basic performance metrics of a DCPerf benchmark run. It is designed to be used in conjunction with the `-k perf` option in DCPerf. PerfPub can:

1. Summarize benchmark result along with key system and performance metrics (e.g. CPU and memory utilization, network traffic, CPU frequency, etc).
2. Output the benchmark result summary in CSV format so that you can directly copy to GSheet.

# Usage of PerfPub

`perfpub [-h] --cpu CPU [--interval INTERVAL] [--last-secs LAST_SECS] [--skip-last-secs SKIP_LAST_SECS] [--note NOTE] [--dir DIR] [--debug]`

`--cpu`: Name of CPU generation (e.g. cpl, milan, bergamo) (Required)

`--interval`: Metrics collection interval (default: 5).

`--last-secs`: Last N seconds of metrics to process as benchmarking stage (default: 300).

`--skip-last-secs`: Skip the last N seconds of metrics (default: 0). This is useful to rule out the final benchmark cleanup phase when the CPU utilization is low and should not be counted in the benchmark execution.

`--note`: Additional note, useful if you have made some special configurations before running the benchmark.

`--dir`: Directory where the benchmark_metrics is located. If not specified, PerfPub will try using the current directory.

An example command line to run PerfPub is:
```bash
./perfpub --cpu bergamo --last-secs 300 --skip-last-secs 30 --dir benchmark_metrics_<run_id>
```

# Recommended `last-secs` and `skip-last-secs` values


| benchmark    | `--laste-secs` | `--skip-last-secs` |
| -------- | ------- | ------- |
| TaoBench | 600 | 120 |
| FeedSim | 300 | 30 |
| DjangoBench | 300 | 60 |
| Mediawiki | 600 | 30 |
| SparkBench (full run) | Value of `execution_time_test_93586` | 10 |
| SparkBench (stage 2.0) | Value of `execution_time_test_93586-stage-2.0` | 10 |
| VideoTranscodeBench | Value of `level6_time_secs` | 10 |
