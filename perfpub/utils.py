#!/usr/bin/python3

# pyre-unsafe

import argparse
import glob
import json
import math
import os
from datetime import datetime

import pandas as pd

default_last_secs = 300
default_skip_last_secs = 0

# Maps CSV filenames to (timestamp_column_name, timestamp_format)
# "time_of_day": absolute time strings like "04:33:45 PM" (mpstat, memstat, etc.)
# "relative_secs": numeric seconds relative to benchmark start (uArch collectors)
# "epoch_secs": absolute Unix epoch timestamps (Intel PerfSpect)
TIMESTAMP_COLUMN_MAP = {
    "mpstat.csv": ("timestamp", "time_of_day"),
    "mem-stat.csv": ("timestamp", "time_of_day"),
    "cpufreq_scaling.csv": ("timestamp", "time_of_day"),
    "cpufreq_cpuinfo.csv": ("timestamp", "time_of_day"),
    "net-stat.csv": ("timestamp", "time_of_day"),
    "perf-stat.csv": ("timestamp", "time_of_day"),
    "amd-perf-collector-timeseries.csv": ("Timestamp_Secs", "relative_secs"),
    "amd-zen4-perf-collector-timeseries.csv": ("Timestamp_Secs", "relative_secs"),
    "amd-zen5-perf-collector-timeseries.csv": ("Timestamp_Secs", "relative_secs"),
    "nv-perf-collector-timeseries.csv": ("Timestamp_Secs", "relative_secs"),
    "nv3-perf-collector-timeseries.csv": ("Timestamp_Secs", "relative_secs"),
    "arm-perf-collector-transposed.csv": ("time", "relative_secs"),
    "topdown-intel.sys.csv": ("TS", "epoch_secs"),
}


def parse_breakdown_csv(breakdown_file, operation_name="main_benchmark"):
    """Parse breakdown.csv to extract operation start and end times.

    This function validates that:
    1. All rows with the specified operation_name have the earliest start and latest end times
    2. For each sub_operation_name with a value, there must be both start and end entries

    Args:
        breakdown_file: Path to breakdown.csv file
        operation_name: Name of the operation to filter by (default: "main_benchmark")

    Returns:
        Tuple of (start_datetime, end_datetime) or (None, None) if not found
    """
    try:
        print(f"parsing {breakdown_file}")
        df = pd.read_csv(breakdown_file)
        operation_rows = df[df["operation_name"] == operation_name]

        if len(operation_rows) == 0:
            print(f"No {operation_name} rows found in breakdown.csv")
            return None, None

        # Validate sub_operation_name entries if the column exists
        if "sub_operation_name" in operation_rows.columns:
            # Get rows with non-null sub_operation_name values
            sub_ops = operation_rows[
                operation_rows["sub_operation_name"].notna()
                & (operation_rows["sub_operation_name"] != "")
            ]

            if len(sub_ops) > 0:
                # Check each unique sub_operation_name has both start and end
                unique_sub_ops = sub_ops["sub_operation_name"].unique()
                for sub_op_name in unique_sub_ops:
                    sub_op_rows = sub_ops[sub_ops["sub_operation_name"] == sub_op_name]
                    has_start = any(sub_op_rows["timestamp_type"] == "start")
                    has_end = any(sub_op_rows["timestamp_type"] == "end")

                    if not has_start or not has_end:
                        print(
                            f"Warning: sub_operation_name '{sub_op_name}' is missing "
                            f"{'start' if not has_start else 'end'} entry"
                        )

        # Find earliest start time across all operation rows
        start_rows = operation_rows[operation_rows["timestamp_type"] == "start"]
        # Find latest end time across all operation rows
        end_rows = operation_rows[operation_rows["timestamp_type"] == "end"]

        if len(start_rows) == 0 or len(end_rows) == 0:
            print("No start/end rows found in breakdown.csv")
            return None, None

        # Parse all start timestamps and find the earliest
        start_times = []
        for start_time_str in start_rows["timestamp"]:
            try:
                start_time = datetime.strptime(start_time_str, "%Y-%m-%d %H:%M:%S.%f")
                start_times.append(start_time)
            except ValueError as e:
                print(f"Error parsing start timestamp '{start_time_str}': {e}")

        # Parse all end timestamps and find the latest
        end_times = []
        for end_time_str in end_rows["timestamp"]:
            try:
                end_time = datetime.strptime(end_time_str, "%Y-%m-%d %H:%M:%S.%f")
                end_times.append(end_time)
            except ValueError as e:
                print(f"Error parsing end timestamp '{end_time_str}': {e}")

        if len(start_times) == 0 or len(end_times) == 0:
            print("Failed to parse any valid timestamps")
            return None, None

        earliest_start = min(start_times)
        latest_end = max(end_times)

        print(f"start_time: {earliest_start}, end_time: {latest_end}")
        return earliest_start, latest_end
    except Exception as e:
        print(f"Error parsing breakdown.csv: {e}")
        return None, None


def parse_metric_timestamps(metric_file):
    """Parse timestamps from a metric file like mpstat.csv.

    Args:
        metric_file: Path to metric file (e.g., mpstat.csv)

    Returns:
        List of datetime objects representing timestamps in the file
    """
    try:
        df = pd.read_csv(metric_file)
        if "timestamp" not in df.columns:
            return []

        timestamps = []
        for ts_str in df["timestamp"]:
            # Parse time-only format like "05:01:49 PM"
            try:
                time_obj = datetime.strptime(ts_str, "%I:%M:%S %p").time()
                timestamps.append(time_obj)
            except ValueError:
                try:
                    # Try alternative format without AM/PM
                    time_obj = datetime.strptime(ts_str, "%H:%M:%S").time()
                    timestamps.append(time_obj)
                except ValueError:
                    continue

        return timestamps
    except Exception as e:
        print(f"Error parsing metric file {metric_file}: {e}")
        return []


def find_closest_timestamp_index(metric_times, target_datetime):
    """Find the index of the timestamp in metric_times closest to target_datetime.

    Args:
        metric_times: List of time objects from metric file
        target_datetime: Target datetime object

    Returns:
        Index of closest timestamp, or None if not found
    """
    if not metric_times:
        return None

    target_time = target_datetime.time()
    min_diff = None
    closest_idx = None

    for idx, metric_time in enumerate(metric_times):
        # Calculate time difference in seconds
        metric_seconds = (
            metric_time.hour * 3600 + metric_time.minute * 60 + metric_time.second
        )
        target_seconds = (
            target_time.hour * 3600 + target_time.minute * 60 + target_time.second
        )

        diff = abs(metric_seconds - target_seconds)

        # Handle day boundary case (e.g., target is 23:59, metric is 00:01)
        if diff > 12 * 3600:  # More than 12 hours difference suggests day boundary
            diff = 86400 - diff  # 86400 seconds in a day

        if min_diff is None or diff < min_diff:
            min_diff = diff
            closest_idx = idx

    return closest_idx


STAGE_PERF_SUMMARY_CSV = "stage_perf_summary.csv"
STAGE_PERF_SUMMARY_JSON = "stage_perf_summary.json"


def _numeric_series(series):
    """Coerce a pandas Series to numeric values, tolerating comma separators."""
    if series.dtype == object:
        series = series.astype(str).str.replace(",", "", regex=False)
    return pd.to_numeric(series, errors="coerce").dropna()


def _summarize_numeric_series(series):
    values = _numeric_series(series)
    if values.empty:
        return None
    return {
        "count": int(values.count()),
        "mean": float(values.mean()),
        "min": float(values.min()),
        "p50": float(values.quantile(0.50)),
        "p95": float(values.quantile(0.95)),
        "max": float(values.max()),
    }


def generate_stage_perf_summary():
    """Generate compact summary artifacts for per-stage perf data.

    The benchpress perf hook's stage-aware mode writes monitor CSVs under
    immediate subdirectories of ``benchmark_metrics_<run_id>/``:

        benchmark_metrics_<run_id>/<stage>/perf-stat.csv
        benchmark_metrics_<run_id>/<stage>/mpstat.csv
        benchmark_metrics_<run_id>/<stage>/nv-perf-collector-summary.csv
        ...

    PerfPub already uploads the whole directory tree to Manifold via
    ``manifold putr``. This helper adds two easy-to-discover top-level
    artifacts before that upload happens:

        stage_perf_summary.csv
        stage_perf_summary.json

    No XDB schema change is required. The summary is intentionally generic:
    for every immediate subdirectory, every CSV file, every numeric column,
    compute count/mean/min/p50/p95/max.

    Returns:
        List of generated summary filenames. Empty when no per-stage CSVs
        are found.
    """
    rows = []
    json_summary = {"stages": {}}

    # Do not recurse into nested timestamp dirs produced by some monitors;
    # stage-aware perf writes stage dirs directly under cwd.
    for stage in sorted(
        d for d in os.listdir(".") if os.path.isdir(d) and not d.startswith(".")
    ):
        csv_files = sorted(
            f
            for f in os.listdir(stage)
            if f.endswith(".csv")
            and f not in {STAGE_PERF_SUMMARY_CSV, STAGE_PERF_SUMMARY_JSON}
        )
        if not csv_files:
            continue

        stage_json = {"files": {}}
        for csv_file in csv_files:
            path = os.path.join(stage, csv_file)
            try:
                df = pd.read_csv(path)
            except Exception as e:
                print(f"Warning: failed to read stage perf CSV {path}: {e}")
                continue

            file_json = {"rows": int(len(df)), "metrics": {}}
            for col in df.columns:
                stats = _summarize_numeric_series(df[col])
                if stats is None:
                    continue
                file_json["metrics"][col] = stats
                rows.append(
                    {
                        "stage": stage,
                        "file": csv_file,
                        "metric": col,
                        **stats,
                    }
                )
            if file_json["metrics"]:
                stage_json["files"][csv_file] = file_json

        if stage_json["files"]:
            json_summary["stages"][stage] = stage_json

    if not rows:
        return []

    with open(STAGE_PERF_SUMMARY_JSON, "w") as f:
        json.dump(json_summary, f, indent=2, sort_keys=True)

    pd.DataFrame(rows).to_csv(STAGE_PERF_SUMMARY_CSV, index=False)
    print(
        f"Generated per-stage perf summaries: {STAGE_PERF_SUMMARY_CSV}, "
        f"{STAGE_PERF_SUMMARY_JSON} ({len(json_summary['stages'])} stages, "
        f"{len(rows)} numeric metrics)"
    )
    return [STAGE_PERF_SUMMARY_CSV, STAGE_PERF_SUMMARY_JSON]


STAGE_OVERALL_METRICS_CSV = "stage_overall_metrics.csv"
STAGE_OVERALL_METRICS_JSON = "stage_overall_metrics.json"
STAGE_OVERALL_METRICS_FILENAME = "overall-metrics.csv"


def _build_stage_overall_metrics_text(stage, bm_metrics, interval):
    """Build an overall-metrics.csv-style text blob for one stage directory.

    This mirrors the normal top-level overall-metrics.csv generated by
    process_metrics(), but is intentionally limited to the perf/sysstat data
    available inside one stage-aware benchmark stage directory. The
    caller must chdir into the stage directory before calling this helper.
    """
    res = ""
    # Include the stage score at the top when the benchmark parser
    # reported one in the top-level metrics JSON.
    try:
        metrics = bm_metrics.get("metrics", {}) if bm_metrics else {}
        if stage in metrics:
            res += f'score,"{metrics[stage]}"\n'
    except Exception:
        pass

    # Stage directories already represent the exact sub-benchmark window, so
    # process the whole CSV (last_secs=0, skip_last_secs=0) instead of slicing
    # by the parent benchmark's breakdown.csv window.
    last_secs = 0
    skip_last_secs = 0
    bm_epoch = None
    start_time = None
    end_time = None

    res += read_mpstat(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_memstat(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_cpufreq_scaling(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_cpufreq_cpuinfo(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_netstat(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_perfstat(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_amd_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_amd_zen4_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_amd_zen5_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_nv_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_arm_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_neoversev3_perf_collector(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    res += read_intel_perfspect(
        interval, last_secs, skip_last_secs, start_time, end_time, bm_epoch
    )
    # Include collector-provided PMU summaries (cache MPKI, TopDown %, BW,
    # latency, etc.) such as nv-perf-collector-summary.csv.
    res += _read_stage_summary_csvs()
    return res


def _read_stage_summary_csvs():
    """Read *-summary.csv files in the current stage directory and emit
    overall-metrics-style key,value lines.

    Stage-aware WDL perf collection produces collector summaries such as
    nv-perf-collector-summary.csv. Those files contain PMU-derived metrics like
    cache MPKI, TopDown BackendBound %, memory bandwidth, etc. The existing
    perfpub readers primarily consume timeseries files, so without this helper
    the per-stage overall-metrics.csv misses the most useful PMU summary rows.

    For each row in each summary CSV, emit only:
      metric,<mean>

    This matches the existing perfpub overall-metrics.csv convention (one
    representative value per metric). The richer count/min/p50/p95/max
    distribution remains available in the top-level stage_perf_summary
    artifacts, not in each stage's overall-metrics.csv.
    """
    res = ""
    summary_files = sorted(
        f
        for f in os.listdir(".")
        if f.endswith("-summary.csv") or f.endswith("_summary.csv")
    )
    for summary_file in summary_files:
        try:
            df = pd.read_csv(summary_file)
        except Exception as e:
            print(f"Warning: failed to read stage summary CSV {summary_file}: {e}")
            continue
        if "metric" not in df.columns or "mean" not in df.columns:
            continue
        for _, row in df.iterrows():
            metric = str(row.get("metric", "")).strip()
            if not metric:
                continue
            # Preserve the simple key for the mean because that's what users
            # expect in overall-metrics.csv.
            mean = row.get("mean")
            if pd.notna(mean):
                res += f'{metric},"{mean}"\n'
    return res


def _parse_overall_metrics_text(text):
    """Parse key,value lines from an overall-metrics.csv-style blob."""
    out = {}
    for line in text.splitlines():
        if not line or "," not in line:
            continue
        key, value = line.split(",", 1)
        key = key.strip()
        value = value.strip().strip('"')
        if not key:
            continue
        out[key] = value
    return out


def generate_stage_overall_metrics(bm_metrics=None, interval=5):
    """Generate per-stage overall-metrics artifacts for WDL prod_set.

    For every immediate subdirectory with perf/sysstat CSV files, write:

        <stage>/overall-metrics.csv

    and also create top-level aggregate indices:

        stage_overall_metrics.csv
        stage_overall_metrics.json

    This gives Manifold users the same processed PMU/sysstat summary they get
    from a normal single-benchmark perfpub run, but scoped to each WDL
    sub-benchmark stage.

    Returns:
        List of generated files (top-level aggregate files plus per-stage
        overall-metrics.csv paths). Empty when no stages are found.
    """
    generated = []
    summary_rows = []
    summary_json = {"stages": {}}
    root = os.getcwd()

    for stage in sorted(
        d for d in os.listdir(".") if os.path.isdir(d) and not d.startswith(".")
    ):
        csv_files = [f for f in os.listdir(stage) if f.endswith(".csv")]
        if not csv_files:
            continue

        try:
            os.chdir(os.path.join(root, stage))
            text = _build_stage_overall_metrics_text(stage, bm_metrics or {}, interval)
        finally:
            os.chdir(root)

        if not text.strip():
            continue

        stage_overall_path = os.path.join(stage, STAGE_OVERALL_METRICS_FILENAME)
        with open(stage_overall_path, "w") as f:
            f.write(text)
        generated.append(stage_overall_path)

        metrics = _parse_overall_metrics_text(text)
        summary_json["stages"][stage] = metrics
        for metric, value in metrics.items():
            summary_rows.append({"stage": stage, "metric": metric, "value": value})

    if not summary_rows:
        return generated

    with open(STAGE_OVERALL_METRICS_JSON, "w") as f:
        json.dump(summary_json, f, indent=2, sort_keys=True)
    pd.DataFrame(summary_rows).to_csv(STAGE_OVERALL_METRICS_CSV, index=False)
    generated.extend([STAGE_OVERALL_METRICS_CSV, STAGE_OVERALL_METRICS_JSON])
    print(
        f"Generated per-stage overall metrics: {STAGE_OVERALL_METRICS_CSV}, "
        f"{STAGE_OVERALL_METRICS_JSON} ({len(summary_json['stages'])} stages, "
        f"{len(summary_rows)} metrics)"
    )
    return generated


def read_benchmark_metrics():
    metrics_jsons = glob.glob("*_metrics_*.json")
    if len(metrics_jsons) == 0:
        return {}
    else:
        with open(metrics_jsons[0]) as f:
            return json.load(f)


def read_system_specs():
    system_specs_jsons = glob.glob("*_system_specs_*.json")
    if len(system_specs_jsons) == 0:
        return {}
    else:
        with open(system_specs_jsons[0]) as f:
            return json.load(f)


def get_bios_version(system_specs):
    res = {}
    dmidecode = system_specs["dmidecode"]
    bios_s = dmidecode["BIOS"]
    if len(bios_s) < 1:
        return {}
    bios = bios_s[0]
    if "Version" in bios:
        res["bios_version"] = bios["Version"]
    if "Release Date" in bios:
        res["bios_release_date"] = bios["Release Date"]
    if "Firmware Revision" in bios:
        res["bios_firmware_revision"] = bios["Firmware Revision"]
    return res


def get_start_end_index(
    df,
    interval,
    last_secs,
    skip_last_secs,
    start_time=None,
    end_time=None,
    ts_column=None,
    ts_format=None,
    start_offset_secs=None,
    end_offset_secs=None,
):
    """Calculate start and end indices for sampling data from a dataframe.

    Supports two timestamp formats:
    - "time_of_day": Absolute time strings (e.g., "04:33:45 PM") in a "timestamp" column
    - "relative_secs": Numeric seconds relative to benchmark start (e.g., Timestamp_Secs)

    Args:
        df: DataFrame with timestamp data
        interval: Metrics collection interval in seconds
        last_secs: Last N seconds to process, or None
        skip_last_secs: Last N seconds to skip, or None
        start_time: Start time in datetime format, or None
        end_time: End time in datetime format, or None
        ts_column: Name of the timestamp column in the CSV
        ts_format: Format type ("time_of_day" or "relative_secs")
        start_offset_secs: Start offset in seconds from benchmark epoch (for relative_secs)
        end_offset_secs: End offset in seconds from benchmark epoch (for relative_secs)

    Returns:
        Tuple of (start_index, end_index)
    """
    # If both parameters are None, use start and end times from breakdown.csv
    if last_secs is None and skip_last_secs is None:
        if start_time is not None and end_time is not None:
            # Handle relative_secs/epoch_secs format (uArch collector CSVs)
            if (
                ts_format in ("relative_secs", "epoch_secs")
                and start_offset_secs is not None
                and end_offset_secs is not None
                and ts_column
                and ts_column in df.columns
            ):
                ts_values = pd.to_numeric(df[ts_column], errors="coerce")
                valid = ts_values.dropna()
                if len(valid) > 0:
                    start_idx = int((valid - start_offset_secs).abs().idxmin())
                    end_idx = int((valid - end_offset_secs).abs().idxmin())
                    return start_idx, end_idx

            # Handle time_of_day format (mpstat, memstat, etc.)
            if "timestamp" in df.columns:
                df_timestamps = []
                for ts_str in df["timestamp"]:
                    try:
                        time_obj = datetime.strptime(ts_str, "%I:%M:%S %p").time()
                        df_timestamps.append(time_obj)
                    except ValueError:
                        try:
                            time_obj = datetime.strptime(ts_str, "%H:%M:%S").time()
                            df_timestamps.append(time_obj)
                        except ValueError:
                            continue

                if df_timestamps:
                    start_idx = find_closest_timestamp_index(df_timestamps, start_time)
                    end_idx = find_closest_timestamp_index(df_timestamps, end_time)

                    if start_idx is not None and end_idx is not None:
                        return start_idx, end_idx

        # If calculation failed or timestamp column doesn't exist, use default values
        last_secs = default_last_secs
        skip_last_secs = default_skip_last_secs

    # Calculate indices using last_secs and skip_last_secs.
    # For CSVs with relative_secs/epoch_secs timestamps, use timestamp-based
    # selection to ensure all CSVs cover the same absolute time window,
    # regardless of per-CSV collection intervals or start-time offsets.
    if (
        ts_format in ("relative_secs", "epoch_secs")
        and ts_column
        and ts_column in df.columns
    ):
        ts_values = pd.to_numeric(df[ts_column], errors="coerce").dropna()
        if len(ts_values) >= 2:
            last_ts = ts_values.iloc[-1]
            end_ts = last_ts - skip_last_secs
            start_ts = end_ts - last_secs
            start_index = int((ts_values - start_ts).abs().idxmin())
            end_index = int((ts_values - end_ts).abs().idxmin())
            start_index = max(start_index, 0)
            end_index = max(end_index, start_index)
            return start_index, end_index

    # Fallback: row-count arithmetic (for time_of_day CSVs or missing timestamp column)
    if last_secs > 0:
        start_index = (
            len(df)
            - math.ceil(last_secs / interval)
            - math.ceil(skip_last_secs / interval)
            - 1
        )
        start_index = max(start_index, 0)
    else:
        start_index = 0
    end_index = len(df) - math.ceil(skip_last_secs / interval) - 1
    return start_index, end_index


def sample_avg_from_csv(
    filename,
    interval,
    last_secs,
    skip_last_secs,
    metrics=(),
    exclude_columns=(),
    div=1,
    key_suffix="",
    sanitizer=None,
    start_time=None,
    end_time=None,
    bm_epoch=None,
):
    try:
        df_mpstat = pd.read_csv(filename, index_col=False)
    except FileNotFoundError:
        return ""

    # Look up timestamp column info for this CSV
    ts_info = TIMESTAMP_COLUMN_MAP.get(os.path.basename(filename))
    ts_column = None
    ts_format = None
    start_offset_secs = None
    end_offset_secs = None
    if ts_info:
        ts_column, ts_format = ts_info
    if start_time is not None and end_time is not None:
        if ts_format == "relative_secs" and bm_epoch is not None:
            start_offset_secs = (start_time - bm_epoch).total_seconds()
            end_offset_secs = (end_time - bm_epoch).total_seconds()
        elif ts_format == "epoch_secs":
            start_offset_secs = start_time.timestamp()
            end_offset_secs = end_time.timestamp()

    start, end = get_start_end_index(
        df_mpstat,
        interval,
        last_secs,
        skip_last_secs,
        start_time,
        end_time,
        ts_column=ts_column,
        ts_format=ts_format,
        start_offset_secs=start_offset_secs,
        end_offset_secs=end_offset_secs,
    )
    print(f"Sampling {filename} from {start} to {end}")
    samples = df_mpstat.iloc[start:end]
    if len(samples) == 0:
        return ""
    samples.to_csv(filename.split(".", maxsplit=1)[0] + ".sampled.csv")
    if metrics:
        present_metrics = [metric for metric in metrics if metric in samples.columns]
        missing_metrics = [
            metric for metric in metrics if metric not in samples.columns
        ]
        if missing_metrics:
            print(f"Columns {missing_metrics} not found in {filename}")
        if not present_metrics:
            return ""
        samples = samples[present_metrics]
    if exclude_columns:
        for excl in exclude_columns:
            if excl in samples:
                del samples[excl]
            else:
                print(f"Column {excl} not found in {filename}")
    if div != 1:
        samples = samples / div
    res = samples.mean()
    if sanitizer:
        res = sanitizer(res)
    if key_suffix:
        res = res.rename({key: key + key_suffix for key in res.index})
    return res.to_csv(header=False)


def read_mpstat(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    metrics = [
        "%gnice",
        "%guest",
        "%idle",
        "%iowait",
        "%irq",
        "%nice",
        "%soft",
        "%steal",
        "%sys",
        "%usr",
    ]
    return sample_avg_from_csv(
        "mpstat.csv",
        interval,
        last_secs,
        skip_last_secs,
        metrics,
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_memstat(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "mem-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        div=1024**3,
        key_suffix="_GB",
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_cpufreq_scaling(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "cpufreq_scaling.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_cpufreq_cpuinfo(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "cpufreq_cpuinfo.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_netstat(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "net-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_perfstat(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "perf-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_amd_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    def sanitize_metrics(series):
        if series.loc["Total Memory Read BW (MB/s)"] < 0:
            series = series.drop(
                ["Total Memory Read BW (MB/s)", "Total Memory Write BW (MB/s)"]
            )
        return series

    return sample_avg_from_csv(
        "amd-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        sanitizer=sanitize_metrics,
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_amd_zen4_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "amd-zen4-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_amd_zen5_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "amd-zen5-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_nv_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "nv-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_arm_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "arm-perf-collector-transposed.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("time",),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_neoversev3_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "nv3-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def read_intel_perfspect(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None, bm_epoch=None
):
    return sample_avg_from_csv(
        "topdown-intel.sys.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("TS",),
        start_time=start_time,
        end_time=end_time,
        bm_epoch=bm_epoch,
    )


def unfold_json(obj, prefix=""):
    res = ""
    if prefix:
        prefix = prefix + "."
    for key, value in obj.items():
        if isinstance(value, dict):
            res += unfold_json(value, prefix + key)
        else:
            res += f'{prefix}{key},"{value}"\n'
    return res


def put_value(db_fields: dict, key: str, kv: str):
    try:
        _, value = kv.split(",")
        db_fields[key] = value
    except ValueError:
        pass


def process_metrics(
    args, additional_processing_on_metrics=None, dump_overall_metrics=None
):
    if args.dir:
        os.chdir(args.dir)

    # Parse breakdown.csv once to get benchmark start and end times
    start_time = None
    end_time = None
    breakdown_path = "breakdown.csv"
    if os.path.exists(breakdown_path):
        start_time, end_time = parse_breakdown_csv(breakdown_path)

    db_fields = {}
    bm_metrics = read_benchmark_metrics()
    if not bm_metrics:
        return ""
    # Generate compact per-stage perf summary artifacts before internal
    # processing uploads the whole benchmark_metrics directory to Manifold.
    # This is a no-op for benchmarks without stage-aware perf subdirectories.
    generate_stage_perf_summary()
    generate_stage_overall_metrics(bm_metrics, args.interval)
    bm_name = bm_metrics["benchmark_name"]
    db_fields["benchmark_name"] = f'"{bm_name}"'
    bm_epoch = datetime.fromtimestamp(bm_metrics["timestamp"])
    timestamp = datetime.strftime(bm_epoch, "%Y-%m-%d %H:%M:%S")
    db_fields["bm_datetime"] = f'"{timestamp}"'
    db_fields["run_id"] = f'"{bm_metrics["run_id"]}"'

    if "score" in bm_metrics["metrics"]:
        db_fields["score"] = bm_metrics["metrics"]["score"]

    # this assigns value to db_fields["metrics"] depending on the benchmark name
    if "feedsim_autoscale" in bm_name:
        db_fields["metrics"] = bm_metrics["metrics"]["overall"]["final_achieved_qps"]
    elif "oss_performance_mediawiki" in bm_name:
        if bm_metrics["metrics"]["Combined"].get("Wrk RPS", None):
            RPS = bm_metrics["metrics"]["Combined"]["Wrk RPS"]
        else:
            RPS = bm_metrics["metrics"]["Combined"]["Siege RPS"]
        db_fields["metrics"] = RPS
    elif "django_workload" in bm_name:
        db_fields["metrics"] = bm_metrics["metrics"]["Transaction rate_trans/sec"]
    elif "tao_bench_autoscale" in bm_name or "tao_bench_standalone" in bm_name:
        db_fields["metrics"] = bm_metrics["metrics"]["total_qps"]
    elif "spark_standalone_remote" in bm_name:
        if "execution_time_test_93586-stage-4.0" in bm_metrics["metrics"]:
            db_fields["metrics"] = bm_metrics["metrics"][
                "execution_time_test_93586-stage-4.0"
            ]
        elif "execution_time_test_93586-stage-2.0" in bm_metrics["metrics"]:
            db_fields["metrics"] = bm_metrics["metrics"][
                "execution_time_test_93586-stage-2.0"
            ]
        else:
            print("No known db metrics found for spark_standalone_remote")

    elif "video_transcode_bench" in bm_name:
        if "throughput_all_levels_hmean_MPxps" in bm_metrics["metrics"]:
            db_fields["metrics"] = bm_metrics["metrics"][
                "throughput_all_levels_hmean_MPxps"
            ]
        else:
            db_fields["metrics"] = bm_metrics["metrics"][
                "throughput_all_levels_hmean_MBps"
            ]
    elif "hackperf" in bm_name:
        if "Requests/sec" in bm_metrics["metrics"]:
            db_fields["metrics"] = bm_metrics["metrics"]["Requests/sec"]
    db_fields["others"] = f"'{json.dumps(bm_metrics['metrics'])}'"

    # This is assigning value to res variable
    res = ""
    # benchmark results
    res += unfold_json(bm_metrics["metrics"])
    # mpstat
    mpstat = read_mpstat(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in mpstat.splitlines():
        if line:
            key, value = line.split(",")
            if "steal" in key or "gnice" in key or "guest" in key:
                continue
            db_fields[key] = value
    res += mpstat

    # memstat
    memstat = read_memstat(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in memstat.splitlines():
        if line:
            key, value = line.split(",")
            db_fields[key] = value
    res += memstat
    # cpufreq
    cpufreq_scaling = read_cpufreq_scaling(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    put_value(db_fields, "cpufreq_mhz_scaling", cpufreq_scaling)
    res += cpufreq_scaling
    cpufreq_cpuinfo = read_cpufreq_cpuinfo(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    put_value(db_fields, "cpufreq_mhz_cpuinfo", cpufreq_cpuinfo)
    res += cpufreq_cpuinfo
    # netstat
    netstat = read_netstat(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in netstat.splitlines():
        if line:
            if "eth0" in line or "lo" in line:
                key, value = line.split(",")
                db_fields[key] = value

    res += netstat
    # perfstat
    perfstat = read_perfstat(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    res += perfstat
    # override cpufreq_mhz_cpuinfo if CPU_CYCLES and CNT_CYCLES exist
    perfstat_kv = {}
    for line in perfstat.splitlines():
        try:
            key, value = line.split(",")
            perfstat_kv[key] = float(value)
        except ValueError:
            continue
    if "CNT_CYCLES" in perfstat_kv and "CPU_CYCLES" in perfstat_kv:
        if args.debug:
            print("Overriding cpufreq_mhz_cpuinfo to CPU_CYCLES/CNT_CYCLES")
        real_measured_freq = (
            1000.0 * perfstat_kv["CPU_CYCLES"] / perfstat_kv["CNT_CYCLES"]
        )
        db_fields["cpufreq_mhz_cpuinfo"] = real_measured_freq

    Map = {
        "Frontend Stalls": "frontend_bound",
        "TopDown FrontendBound %": "frontend_bound",
        "Topdown Level 1/Frontend Bound": "frontend_bound",
        "metric_TMA_Frontend_Bound(%)": "frontend_bound",
        "Frontend Bound %": "frontend_bound",
        "Topdown Level 1/Backend Bound": "backend_bound",
        "Backend Stalls": "backend_bound",
        "TopDown BackendBound %": "backend_bound",
        "metric_TMA_Backend_Bound(%)": "backend_bound",
        "Backend Bound %": "backend_bound",
        "Avg. IPC": "IPC",
        "IPC": "IPC",
        "General/Instructions Per Cycle": "IPC",
        "metric_IPC": "IPC",
        "Branch Mispred %": "branch_mispred",
        "Branch Effectiveness/Branch Misprediction Ratio": "branch_mispred",
        "metric_TMA_..Branch_Mispredicts(%)": "branch_mispred",
        "Branch Retired Mispred PKI": "branch_mispred",
        "L1 ICache MPKI (w/ prefetches)": "L1_icache_mpki",
        "L1 ICache MPKI": "L1_icache_mpki",
        "L1 Instruction Cache Effectiveness/L1I Cache MPKI": "L1_icache_mpki",
        "metric_L1-I code read misses (w/ prefetches) per instr": "L1_icache_mpki",
        "Any L1 IC Fills PKI": "L1_icache_mpki",
        "L1 DCache MPKI (w/ prefetches)": "L1_dcache_mpki",
        "L1 DCache MPKI": "L1_dcache_mpki",
        "Misses Per Kilo Instructions/L1D Cache MPKI": "L1_dcache_mpki",
        "metric_L1D MPI (includes data+rfo w/ prefetches)": "L1_dcache_mpki",
        "Any L1 DC Fills PKI": "L1_dcache_mpki",
        "L2 Cache MPKI": "L2_cache_mpki",
        "L2 Unified Cache Effectiveness/L2 Cache MPKI": "L2_cache_mpki",
        "metric_L2 MPI (includes code+data+rfo w/ prefetches)": "L2_cache_mpki",
        "LLC MPKI": "LLC_mpki",
        "L3 Cache MPKI": "LLC_mpki",
        "Last Level Cache Effectiveness/LL Cache Read MPKI": "LLC_mpki",
        "metric_LLC MPI (includes code+data+rfo w/ prefetches)": "LLC_mpki",
        "iTLB MPKI": "iTLB_mpki",
        "iTLB Walk MPKI": "iTLB_mpki",
        "Instruction TLB Effectiveness/ITLB MPKI": "iTLB_mpki",
        "metric_ITLB MPI": "iTLB_mpki",
        "L1 iTLB Miss PKI": "iTLB_mpki",
        "dTLB MPKI": "dTLB_mpki",
        "dTLB Walk MPKI": "dTLB_mpki",
        "Data TLB Effectiveness/DTLB MPKI": "dTLB_mpki",
        "metric_DTLB load MPI": "dTLB_mpki",
        "L1 dTLB Miss PKI": "dTLB_mpki",
        "TopDown Retiring %": "retiring",
        "Topdown Level 1/Retiring": "retiring",
        "metric_TMA_Retiring(%)": "retiring",
        "Retiring %": "retiring",
        "Topdown Level 1/Bad Speculation": "bad_speculation",
        "metric_TMA_Bad_Speculation(%)": "bad_speculation",
        "TopDown bad Speculation %": "bad_speculation",
        "TopDown Bad Speculation %": "bad_speculation",
        "Bad Speculation %": "bad_speculation",
        "L1 iCache MPKI": "L1_icache_mpki",
        "L1 dCache MPKI": "L1_dcache_mpki",
        "L2 Code MPKI": "L2_code_mpki",
        "metric_L2 demand code MPI": "L2_code_mpki",
        "L2 Data MPKI": "L2_data_mpki",
        "metric_L2 demand data read MPI": "L2_data_mpki",
    }

    amd_perf_collector = read_amd_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in amd_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += amd_perf_collector
    amd_zen4_perf_collector = read_amd_zen4_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in amd_zen4_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen4_perf_collector

    amd_zen5_perf_collector = read_amd_zen5_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in amd_zen5_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen5_perf_collector

    nv_perf_collector = read_nv_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in nv_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += nv_perf_collector
    arm_perf_collector = read_arm_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in arm_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += arm_perf_collector
    nv3_perf_collector = read_neoversev3_perf_collector(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    for line in nv3_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += nv3_perf_collector
    intel_perfspect = read_intel_perfspect(
        args.interval,
        args.last_secs,
        args.skip_last_secs,
        start_time,
        end_time,
        bm_epoch,
    )
    res += intel_perfspect
    for line in intel_perfspect.splitlines():
        key, value = line.split(",")
        if key in Map:
            if "mpki" in Map[key]:
                value = float(value) * 1000
            db_fields[f"{Map[key]}"] = value

    # machine info
    res += "machine,\n"
    for key, value in bm_metrics["machines"][0].items():
        res += f'{key},"{value}"\n'
        db_fields[key] = f'"{value}"'
    for key, value in bm_metrics["metadata"].items():
        res += f'{key},"{value}"\n'
        db_fields[key] = f'"{value}"'

    # bios info
    system_specs = read_system_specs()
    bios_info = get_bios_version(system_specs)
    res += unfold_json(bios_info)
    bios_version = bios_info["bios_version"] if "bios_version" in bios_info else ""
    bios_rel_date = (
        bios_info["bios_release_date"] if "bios_release_date" in bios_info else ""
    )
    db_fields["bios_version"] = f'"{bios_version}"'
    db_fields["bios_release_date"] = f'"{bios_rel_date}"'

    # other input
    db_fields["cpu_generation"] = f'"{args.cpu}"'
    db_fields["note"] = f'"{args.note}"'

    if "version_info" in bm_metrics:
        db_fields["version_source"] = f'"{bm_metrics["version_info"]["source"]}"'
        res += f"version_source,{db_fields['version_source']}\n"
        db_fields["version_uuid"] = f'"{bm_metrics["version_info"]["uuid"]}"'
        res += f"version_uuid,{db_fields['version_uuid']}\n"
        db_fields["version"] = f'"{bm_metrics["version_info"]["version"]}"'
        res += f"version,{db_fields['version']}\n"

    # benchmark args
    res += "benchmark args,\n"
    values_benchmarks_args = ", ".join(bm_metrics["benchmark_args"])
    for arg in bm_metrics["benchmark_args"]:
        res += f',"{arg}"\n'
    db_fields["benchmark args"] = f'"{values_benchmarks_args}"'

    if additional_processing_on_metrics is not None:
        res = additional_processing_on_metrics(
            args, db_fields, res, bm_metrics, dump_overall_metrics
        )
    elif dump_overall_metrics is not None:
        dump_overall_metrics(res)


def dump_overall_metrics(res):
    print(res)
    with open("overall-metrics.csv", "w") as f:
        f.write(res)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument(
        "--cpu",
        type=str,
        default="",
        help="Name of CPU generation (e.g. cpl, milan, bergamo). Can be omitted if --auto-detect-cpu is used.",
    )
    parser.add_argument(
        "--interval", type=int, default=5, help="Metrics collection interval"
    )
    parser.add_argument(
        "--last-secs",
        type=int,
        default=None,
        help=f'Last N seconds of metrics to process as benchmarking stage. If not provided and breakdown.csv is not present, use default of {default_last_secs}. Recommended value: Taobench: 600; Feedsim: 300; Spark (full run): value of "execution_time_test_93586"; Spark (stage 2.0): value of "execution_time_test_93586-stage-2.0"; Video Transcode: value of "level6_time_secs"; Django: 300; MediaWiki: 600',
    )
    parser.add_argument(
        "--skip-last-secs",
        type=int,
        default=None,
        help=f"Skip the last N seconds of metrics. If not provided and breakdown.csv is not present, use default of {default_skip_last_secs}. Recommended value: Taobench: 120; Feedsim: 30; Spark: 10; Video Transcode: 10; Django: 60; MediaWiki: 30",
    )
    parser.add_argument(
        "--note", type=str, default="", help="Additional note to be added to the folder"
    )
    parser.add_argument(
        "--dir",
        type=str,
        default="",
        help="Directory where the benchmark_metrics is located",
    )
    return parser
