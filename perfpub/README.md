# PerfPub

PerfPub is a convenient tool that analyzes and summarizes basic performance metrics of a DCPerf benchmark run. It is designed to be used in conjunction with the `-k perf` option in DCPerf. PerfPub can:

1. Summarize benchmark result along with key system and performance metrics (e.g. CPU and memory utilization, network traffic, CPU frequency, etc).
2. Output the benchmark result summary in CSV format so that you can directly copy to GSheet.

# Usage of PerfPub

```
perfpub [-h] [--cpu CPU] [--auto-detect-cpu] [--interval INTERVAL] [--last-secs LAST_SECS] [--skip-last-secs SKIP_LAST_SECS] [--note NOTE] [--dir DIR] [--experiment-folder EXPERIMENT_FOLDER] [--cpu-model-mapping CPU_MODEL_MAPPING] [--debug]
```

## CPU Detection

PerfPub can determine the CPU name in two ways (in order of priority):


1. **Explicit `--cpu` argument**: Manually specify the CPU generation (e.g., `cpl`, `milan`, `bergamo`, `grace`, `turin`).

2. **Auto-detect from system specs** (`--auto-detect-cpu`): Reads the CPU model from the `system_specs.json` file in the metrics directory and maps it to a friendly name using a built-in or custom mapping file.

## Arguments

`--cpu`: Name of CPU generation (e.g. cpl, milan, bergamo, grace, turin). Optional - if not specified, PerfPub will auto-detect the CPU.

`--auto-detect-cpu`: Automatically detect CPU name from the `system_specs.json` file in the metrics directory.

`--cpu-model-mapping`: Path to a custom JSON file mapping CPU model names to friendly names. Used with `--auto-detect-cpu`.

`--interval`: Metrics collection interval (default: 5).

`--last-secs`: Last N seconds of metrics to process as benchmarking stage (default: 300). **Note:** For benchmarks that have integrated `breakdown.csv`, this parameter is not needed as PerfPub will automatically determine the benchmark start and end time (see [Automatic Metric Filtering with breakdown.csv](#automatic-metric-filtering-with-breakdowncsv)).

`--skip-last-secs`: Skip the last N seconds of metrics (default: 0). This is useful to rule out the final benchmark cleanup phase when the CPU utilization is low and should not be counted in the benchmark execution. **Note:** For benchmarks that have integrated `breakdown.csv`, this parameter is not needed.

`--note`: Additional note, useful if you have made some special configurations before running the benchmark.

`--dir`: Directory where the benchmark_metrics is located. If not specified, PerfPub will try using the current directory.

`--experiment-folder`: Experiment folder name to organize results in Manifold (e.g., `my_experiment`). Results will be stored under `experiment_folder/benchmark_name/cpu_name/`.


# Recommended `last-secs` and `skip-last-secs` values

**Note:** For benchmarks that have integrated `breakdown.csv`, you do not need to specify `--last-secs` and `--skip-last-secs`. PerfPub will automatically determine the benchmark start and end time from the breakdown file.

For benchmarks without `breakdown.csv` integration, use the following values:

| benchmark    | `--last-secs` | `--skip-last-secs` |
| -------- | ------- | ------- |
| TaoBench | 600 | 120 |
| FeedSim | 300 | 30 |
| DjangoBench | 300 | 60 |
| Mediawiki | 600 | 30 |
| SparkBench (full run) | Value of `execution_time_test_93586` | 10 |
| SparkBench (stage 2.0) | Value of `execution_time_test_93586-stage-2.0` | 10 |
| VideoTranscodeBench | Value of `level6_time_secs` | 10 |

# Automatic Metric Filtering with breakdown.csv

PerfPub can automatically determine the exact benchmark execution window using the `breakdown.csv` file. This is particularly useful for DCPerf mini benchmarks where the benchmark duration is only a few seconds and precise metric filtering is essential.

## How It Works

The metric filtering logic follows this priority order:

1. **User-provided values**: If `--last-secs` and `--skip-last-secs` are explicitly provided, they will be used for filtering metrics.

2. **Automatic detection via breakdown.csv**: If timing parameters are not provided and `breakdown.csv` is present in the metrics directory, PerfPub will:
   - Parse the `breakdown.csv` file to find the `main_benchmark` operation
   - Extract the earliest start timestamp and latest end timestamp
   - Automatically filter metrics to match the exact benchmark execution window

3. **Default values**: If `breakdown.csv` is not present and no timing parameters are provided, PerfPub uses defaults (`--last-secs=300`, `--skip-last-secs=0`).

## breakdown.csv Format

The `breakdown.csv` file is generated during benchmark runs and contains timing information for different benchmark phases. It includes the following columns:

| Column | Description |
| ------ | ----------- |
| `operation_name` | Name of the operation (e.g., `main_benchmark`) |
| `timestamp_type` | Either `start` or `end` |
| `timestamp` | Timestamp in format `YYYY-MM-DD HH:MM:SS.fff` |
| `sub_operation_name` | (Optional) Name of sub-operation for more granular timing |

### Example breakdown.csv

```csv
operation_name,timestamp_type,timestamp,sub_operation_name
main_benchmark,start,2025-10-16 16:59:21.909,
main_benchmark,end,2025-10-16 17:04:32.123,
main_benchmark,start,2025-10-16 16:59:25.000,warmup
main_benchmark,end,2025-10-16 16:59:45.000,warmup
main_benchmark,start,2025-10-16 16:59:45.100,execution
main_benchmark,end,2025-10-16 17:04:30.000,execution
```

## Sub-operation Validation

When `breakdown.csv` contains `sub_operation_name` entries, PerfPub validates that each sub-operation has both a `start` and `end` entry. If either is missing, a warning is printed.

## Example Output

When PerfPub uses `breakdown.csv` for filtering, you'll see output like:

```
parsing breakdown.csv
start_time: 2025-10-16 16:59:21.909, end_time: 2025-10-16 17:04:32.123
Sampling mpstat.csv from 7 to 17
Sampling mem-stat.csv from 8 to 18
...
```

This shows the exact benchmark window and the row ranges used for each metric file.
