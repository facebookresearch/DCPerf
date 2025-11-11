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
    df, interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    """Calculate start and end indices for sampling data from a dataframe.

    If last_secs and skip_last_secs are None, attempts to calculate them from breakdown.csv
    by finding the closest timestamps in the dataframe's timestamp column.

    Args:
        df: DataFrame with a 'timestamp' column
        interval: Metrics collection interval in seconds
        last_secs: Last N seconds to process, or None
        skip_last_secs: Last N seconds to skip, or None
        start_time: Start time in datetime format, or None
        end_time: End time in datetime format, or None

    Returns:
        Tuple of (start_index, end_index)
    """
    # If both parameters are None, use start and end times from breakdown.csv
    if last_secs is None and skip_last_secs is None:
        if start_time is not None and end_time is not None:
            # Check if timestamp column exists
            if "timestamp" in df.columns:
                # Get timestamps from the current dataframe
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
                    # Find closest timestamps in this dataframe
                    start_idx = find_closest_timestamp_index(df_timestamps, start_time)
                    end_idx = find_closest_timestamp_index(df_timestamps, end_time)

                    if start_idx is not None and end_idx is not None:
                        return start_idx, end_idx

        # If calculation failed or timestamp column doesn't exist, use default values
        last_secs = default_last_secs
        skip_last_secs = default_skip_last_secs

    # Calculate indices using last_secs and skip_last_secs
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
):
    try:
        df_mpstat = pd.read_csv(filename, index_col=False)
    except FileNotFoundError:
        return ""
    start, end = get_start_end_index(
        df_mpstat, interval, last_secs, skip_last_secs, start_time, end_time
    )
    print(f"Sampling {filename} from {start} to {end}")
    samples = df_mpstat.iloc[start:end]
    if len(samples) == 0:
        return ""
    samples.to_csv(filename.split(".", maxsplit=1)[0] + ".sampled.csv")
    if metrics:
        samples = samples[metrics]
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


def read_mpstat(interval, last_secs, skip_last_secs, start_time=None, end_time=None):
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
    )


def read_memstat(interval, last_secs, skip_last_secs, start_time=None, end_time=None):
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
    )


def read_cpufreq_scaling(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "cpufreq_scaling.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
    )


def read_cpufreq_cpuinfo(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "cpufreq_cpuinfo.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
    )


def read_netstat(interval, last_secs, skip_last_secs, start_time=None, end_time=None):
    return sample_avg_from_csv(
        "net-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
    )


def read_perfstat(interval, last_secs, skip_last_secs, start_time=None, end_time=None):
    return sample_avg_from_csv(
        "perf-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        start_time=start_time,
        end_time=end_time,
    )


def read_amd_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
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
    )


def read_amd_zen4_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "amd-zen4-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
    )


def read_amd_zen5_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "amd-zen5-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
    )


def read_nv_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "nv-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
        start_time=start_time,
        end_time=end_time,
    )


def read_arm_perf_collector(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "arm-perf-collector-transposed.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("time",),
        start_time=start_time,
        end_time=end_time,
    )


def read_intel_perfspect(
    interval, last_secs, skip_last_secs, start_time=None, end_time=None
):
    return sample_avg_from_csv(
        "topdown-intel.sys.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("TS",),
        start_time=start_time,
        end_time=end_time,
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


def get_server_info(param):
    """
    Query comprehensive server information given a hostname or asset_id

    Args:
        param (str): The hostname of the server to query or asset_id

    Returns:
        dict: Dictionary containing Asset ID, LSST, model info, CPU info, and evaluation status
    """

    from ame.serf.clients.py.serf import Serf3ServiceClient
    from facebook.core_systems.device.ttypes import EvaluationStatus
    from facebook.core_systems.logical_server_type.types import LogicalServerSubType
    from facebook.core_systems.queries.ttypes import BinaryExpression, Operator, Query

    with Serf3ServiceClient() as serf_client:
        # Check if param is a digit (asset_id) or hostname
        if param.isdigit():
            # Query by asset_id
            query_field = "asset_id"
            query_value = param
        else:
            # Query by hostname
            query_field = "name"
            query_value = param

        devices = serf_client.getDevices(
            query=Query(
                binExprs=[BinaryExpression(query_field, Operator.EQUAL, [query_value])]
            ),
            columns=[
                "asset_id",
                "logical_server_subtype",
                "model_id",
                "evaluation_status",
                "name",
            ],
        )

        if not devices:
            return {"error": f"No device found with {query_field}: {param}"}

        device = devices[0]

        # Convert LSST ID to human-readable name
        try:
            lsst_name = (
                LogicalServerSubType(device.logical_server_subtype).name
                if device.logical_server_subtype
                else "LSST_UNKNOWN"
            )
        except Exception:
            lsst_name = f"LSST_ID_{device.logical_server_subtype}"

        # Convert evaluation_status to human-readable name
        try:
            eval_status_name = (
                EvaluationStatus._VALUES_TO_NAMES.get(
                    device.evaluation_status, f"EvalStatus_{device.evaluation_status}"
                )
                if hasattr(device, "evaluation_status")
                else "EVAL_UNKNOWN"
            )
            if eval_status_name.startswith("EVAL_"):
                eval_status_name = eval_status_name[5:]

        except Exception:
            eval_status_name = f"EvalStatus_{device.evaluation_status if hasattr(device, 'evaluation_status') else 'EVAL_UNKNOWN'}"

        # Query model name from model_id
        model_name = "MODEL_NAME_UNKNOWN"
        model_make = "MODEL_MAKE_UNKNOWN"
        if device.model_id:
            try:
                models = serf_client.getModels(
                    query=Query(
                        binExprs=[
                            # pyre-ignore
                            BinaryExpression(
                                "id", Operator.EQUAL, [str(device.model_id)]
                            )
                        ]
                    ),
                    columns=["model", "make"],
                )
                if models:
                    model_name = (
                        models[0].model if models[0].model else "MODEL_NAME_UNKNOWN"
                    )
                    model_make = (
                        models[0].make if models[0].make else "MODEL_MAKE_UNKNOWN"
                    )
            except Exception:
                model_name = f"ModelID_{device.model_id}"

        # Query CPU information from components
        cpu_architecture = "Unknown"

        try:
            # Get active CPU components
            cpu_components = serf_client.getComponents(
                query=Query(
                    binExprs=[
                        BinaryExpression("device_id", Operator.EQUAL, [str(device.id)]),
                        BinaryExpression("type", Operator.EQUAL, ["serf.CPU"]),
                        BinaryExpression("active", Operator.EQUAL, ["1"]),
                    ]
                ),
                columns=["fbpn"],
            )

            if (
                cpu_components
                and cpu_components[0].fbpn
                and cpu_components[0].fbpn != "Unknown"
            ):
                # Query part information - use "*" to get all fields
                parts = serf_client.getParts(
                    query=Query(
                        binExprs=[
                            BinaryExpression(
                                "fbpn", Operator.EQUAL, [cpu_components[0].fbpn]
                            )
                        ]
                    ),
                    columns=["*"],
                )

                if parts:
                    part = parts[0]

                    # Get CPU model from manufacturers
                    if hasattr(part, "manufacturers") and part.manufacturers:
                        cpu_model = part.manufacturers[0].model
                        # Extract architecture from model name (first words before core count)
                        # e.g., "Cooper Lake 26C" -> "COOPER_LAKE"
                        # e.g., "Bergamo 88C" -> "BERGAMO"
                        if cpu_model:
                            # Remove trailing core count pattern (e.g., "26C", "88C")
                            model_parts = cpu_model.split()
                            # Filter out parts that look like core counts (number + C)
                            arch_parts = [
                                p
                                for p in model_parts
                                if not (p.endswith("C") and p[:-1].isdigit())
                            ]
                            # Convert to uppercase and replace spaces with underscores
                            cpu_architecture = (
                                "_".join(arch_parts).upper()
                                if arch_parts
                                else cpu_model.upper()
                            )

        except Exception:
            # If parts query fails, architecture remains "Unknown"
            pass

        return {
            "hostname": device.name,
            "asset_id": (
                device.asset_id
                if hasattr(device, "asset_id") and device.asset_id
                else device.id
            ),
            "lsst": lsst_name,
            "model_name": model_name,
            "model_make": model_make,
            "cpu_generation": cpu_architecture,
            "evaluation_status": eval_status_name,
        }


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

    columns = "("
    # values = "("
    db_fields = {}
    bm_metrics = read_benchmark_metrics()
    if not bm_metrics:
        return ""
    bm_name = bm_metrics["benchmark_name"]
    db_fields["benchmark_name"] = f'"{bm_name}"'
    timestamp = datetime.strftime(
        datetime.fromtimestamp(bm_metrics["timestamp"]), "%Y-%m-%d %H:%M:%S"
    )
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
        db_fields["metrics"] = bm_metrics["metrics"]["throughput_all_levels_hmean_MBps"]
    db_fields["others"] = f"'{json.dumps(bm_metrics['metrics'])}'"

    # This is assigning value to res variable
    res = ""
    # benchmark results
    res += unfold_json(bm_metrics["metrics"])
    # mpstat
    mpstat = read_mpstat(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
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
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in memstat.splitlines():
        if line:
            key, value = line.split(",")
            db_fields[key] = value
    res += memstat
    # cpufreq
    cpufreq_scaling = read_cpufreq_scaling(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    put_value(db_fields, "cpufreq_mhz_scaling", cpufreq_scaling)
    res += cpufreq_scaling
    cpufreq_cpuinfo = read_cpufreq_cpuinfo(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    put_value(db_fields, "cpufreq_mhz_cpuinfo", cpufreq_cpuinfo)
    res += cpufreq_cpuinfo
    # netstat
    netstat = read_netstat(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in netstat.splitlines():
        if line:
            if "eth0" in line or "lo" in line:
                key, value = line.split(",")
                db_fields[key] = value

    res += netstat
    # perfstat
    perfstat = read_perfstat(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
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
        "Topdown Level 1/Backend Bound": "backend_bound",
        "Backend Stalls": "backend_bound",
        "TopDown BackendBound %": "backend_bound",
        "metric_TMA_Backend_Bound(%)": "backend_bound",
        "Avg. IPC": "IPC",
        "IPC": "IPC",
        "General/Instructions Per Cycle": "IPC",
        "metric_IPC": "IPC",
        "Branch Mispred %": "branch_mispred",
        "Branch Effectiveness/Branch Misprediction Ratio": "branch_mispred",
        "metric_TMA_..Branch_Mispredicts(%)": "branch_mispred",
        "L1 ICache MPKI (w/ prefetches)": "L1_icache_mpki",
        "L1 ICache MPKI": "L1_icache_mpki",
        "L1 Instruction Cache Effectiveness/L1I Cache MPKI": "L1_icache_mpki",
        "metric_L1-I code read misses (w/ prefetches) per instr": "L1_icache_mpki",
        "L1 DCache MPKI (w/ prefetches)": "L1_dcache_mpki",
        "L1 DCache MPKI": "L1_dcache_mpki",
        "Misses Per Kilo Instructions/L1D Cache MPKI": "L1_dcache_mpki",
        "metric_L1D MPI (includes data+rfo w/ prefetches)": "L1_dcache_mpki",
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
        "dTLB MPKI": "dTLB_mpki",
        "dTLB Walk MPKI": "dTLB_mpki",
        "Data TLB Effectiveness/DTLB MPKI": "dTLB_mpki",
        "metric_DTLB load MPI": "dTLB_mpki",
        "TopDown Retiring %": "retiring",
        "Topdown Level 1/Retiring": "retiring",
        "metric_TMA_Retiring(%)": "retiring",
        "Topdown Level 1/Bad Speculation": "bad_speculation",
        "metric_TMA_Bad_Speculation(%)": "bad_speculation",
        "L2 Code MPKI": "L2_code_mpki",
        "metric_L2 demand code MPI": "L2_code_mpki",
        "L2 Data MPKI": "L2_data_mpki",
        "metric_L2 demand data read MPI": "L2_data_mpki",
    }

    amd_perf_collector = read_amd_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in amd_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += amd_perf_collector
    amd_zen4_perf_collector = read_amd_zen4_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in amd_zen4_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen4_perf_collector

    amd_zen5_perf_collector = read_amd_zen5_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in amd_zen5_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen5_perf_collector

    nv_perf_collector = read_nv_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in nv_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += nv_perf_collector
    arm_perf_collector = read_arm_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
    )
    for line in arm_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += arm_perf_collector
    intel_perfspect = read_intel_perfspect(
        args.interval, args.last_secs, args.skip_last_secs, start_time, end_time
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
    db_fields["note"] = f'"{args.note}"'

    if "version_info" in bm_metrics:
        db_fields["version_source"] = f'"{bm_metrics["version_info"]["source"]}"'
        res += f"version_source,{db_fields["version_source"]}\n"
        db_fields["version_uuid"] = f'"{bm_metrics["version_info"]["uuid"]}"'
        res += f"version_uuid,{db_fields["version_uuid"]}\n"
        db_fields["version"] = f'"{bm_metrics["version_info"]["version"]}"'
        res += f"version,{db_fields["version"]}\n"

    # benchmark args
    res += "benchmark args,\n"
    values_benchmarks_args = ", ".join(bm_metrics["benchmark_args"])
    for arg in bm_metrics["benchmark_args"]:
        res += f',"{arg}"\n'
    db_fields["benchmark args"] = f'"{values_benchmarks_args}"'

    # server info from Serf3
    hostname = bm_metrics["machines"][0].get("hostname", "")
    if hostname.endswith(".facebook.com") or args.asset_id:
        if args.asset_id:
            param = args.asset_id
        else:
            param = hostname
        server_info = get_server_info(param)

        lsst = server_info.get("lsst", "") if not args.lsst else args.lsst
        asset_id = (
            server_info.get("asset_id", "") if not args.asset_id else args.asset_id
        )
        model_name = (
            server_info.get("model_name", "")
            if not args.model_name
            else args.model_name
        )
        model_make = (
            server_info.get("model_make", "")
            if not args.model_make
            else args.model_make
        )
        evaluation_status = (
            server_info.get("evaluation_status", "")
            if not args.evaluation_status
            else args.evaluation_status
        )
        cpu_generation = server_info.get("cpu_generation", "")

        if "hostname" not in res:
            hostname = server_info.get("hostname", "")
            res += f'hostname,"{hostname}"\n'

        db_fields["lsst"] = f'"{lsst}"'
        db_fields["asset_id"] = f'"{asset_id}"'
        db_fields["model_name"] = f'"{model_name}"'
        db_fields["model_make"] = f'"{model_make}"'
        db_fields["evaluation_status"] = f'"{evaluation_status}"'
        db_fields["cpu_generation"] = f'"{cpu_generation}"'

        res += f'lsst,"{lsst}"\n'
        res += f'asset_id,"{asset_id}"\n'
        res += f'model_name,"{model_name}"\n'
        res += f'model_make,"{model_make}"\n'
        res += f'evaluation_status,"{evaluation_status}"\n'

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
    parser.add_argument(
        "--lsst",
        type=str,
        default="",
        help="Logical Server SubType (LSST) - overrides Serf3 API value if provided",
    )
    parser.add_argument(
        "--asset-id",
        type=str,
        default="",
        help="asset_id - overrides Serf3 API value if provided",
    )

    parser.add_argument(
        "--model-name",
        type=str,
        default="",
        help="Server model name - overrides Serf3 API value if provided",
    )
    parser.add_argument(
        "--model-make",
        type=str,
        default="",
        help="Server model make/manufacturer - overrides Serf3 API value if provided",
    )
    parser.add_argument(
        "--evaluation-status",
        type=str,
        default="",
        help="Server evaluation status - overrides Serf3 API value if provided",
    )
    return parser
