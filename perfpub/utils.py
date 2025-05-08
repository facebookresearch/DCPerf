#!/usr/bin/python3

# pyre-unsafe

import argparse
import glob
import json
import math
import os
from datetime import datetime

import pandas as pd


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


def get_start_end_index(df, interval, last_secs, skip_last_secs):
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
):
    try:
        df_mpstat = pd.read_csv(filename, index_col=False)
    except FileNotFoundError:
        return ""
    start, end = get_start_end_index(df_mpstat, interval, last_secs, skip_last_secs)
    samples = df_mpstat.iloc[start:end]
    if len(samples) == 0:
        return ""
    samples.to_csv(filename.split(".", maxsplit=1)[0] + ".sampled.csv")
    if metrics:
        samples = samples[metrics]
    if exclude_columns:
        for excl in exclude_columns:
            del samples[excl]
    if div != 1:
        samples = samples / div
    res = samples.mean()
    if sanitizer:
        res = sanitizer(res)
    if key_suffix:
        res = res.rename({key: key + key_suffix for key in res.index})
    return res.to_csv(header=False)


def read_mpstat(interval, last_secs, skip_last_secs):
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
        "mpstat.csv", interval, last_secs, skip_last_secs, metrics
    )


def read_memstat(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "mem-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
        div=1024**3,
        key_suffix="_GB",
    )


def read_cpufreq_scaling(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "cpufreq_scaling.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
    )


def read_cpufreq_cpuinfo(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "cpufreq_cpuinfo.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
    )


def read_netstat(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "net-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
    )


def read_perfstat(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "perf-stat.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "timestamp"),
    )


def read_amd_perf_collector(interval, last_secs, skip_last_secs):
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
    )


def read_amd_zen4_perf_collector(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "amd-zen4-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
    )


def read_amd_zen5_perf_collector(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "amd-zen5-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
    )


def read_nv_perf_collector(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "nv-perf-collector-timeseries.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("index", "Timestamp_Secs"),
    )


def read_arm_perf_collector(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "arm-perf-collector-transposed.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("time",),
    )


def read_intel_perfspect(interval, last_secs, skip_last_secs):
    return sample_avg_from_csv(
        "topdown-intel.sys.csv",
        interval,
        last_secs,
        skip_last_secs,
        exclude_columns=("time",),
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
        db_fields["metrics"] = bm_metrics["metrics"][
            "execution_time_test_93586-stage-2.0"
        ]
    elif "video_transcode_bench" in bm_name:
        db_fields["metrics"] = bm_metrics["metrics"]["throughput_all_levels_hmean_MBps"]
    db_fields["others"] = f"'{json.dumps(bm_metrics['metrics'])}'"

    # This is assigning value to res variable
    res = ""
    # benchmark results
    res += unfold_json(bm_metrics["metrics"])
    # mpstat
    mpstat = read_mpstat(args.interval, args.last_secs, args.skip_last_secs)
    for line in mpstat.splitlines():
        if line:
            key, value = line.split(",")
            if "steal" in key or "gnice" in key or "guest" in key:
                continue
            db_fields[key] = value
    res += mpstat

    # memstat
    memstat = read_memstat(args.interval, args.last_secs, args.skip_last_secs)
    for line in memstat.splitlines():
        if line:
            key, value = line.split(",")
            db_fields[key] = value
    res += memstat
    # cpufreq
    cpufreq_scaling = read_cpufreq_scaling(
        args.interval, args.last_secs, args.skip_last_secs
    )
    put_value(db_fields, "cpufreq_mhz_scaling", cpufreq_scaling)
    res += cpufreq_scaling
    cpufreq_cpuinfo = read_cpufreq_cpuinfo(
        args.interval, args.last_secs, args.skip_last_secs
    )
    put_value(db_fields, "cpufreq_mhz_cpuinfo", cpufreq_cpuinfo)
    res += cpufreq_cpuinfo
    # netstat
    netstat = read_netstat(args.interval, args.last_secs, args.skip_last_secs)
    for line in netstat.splitlines():
        if line:
            if "eth0" in line or "lo" in line:
                key, value = line.split(",")
                db_fields[key] = value

    res += netstat
    # perfstat
    perfstat = read_perfstat(args.interval, args.last_secs, args.skip_last_secs)
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
        args.interval, args.last_secs, args.skip_last_secs
    )
    for line in amd_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += amd_perf_collector
    amd_zen4_perf_collector = read_amd_zen4_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs
    )
    for line in amd_zen4_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen4_perf_collector

    amd_zen5_perf_collector = read_amd_zen5_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs
    )
    for line in amd_zen5_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map and Map[key] not in db_fields.keys():
            db_fields[f"{Map[key]}"] = value
    res += amd_zen5_perf_collector

    nv_perf_collector = read_nv_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs
    )
    for line in nv_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += nv_perf_collector
    arm_perf_collector = read_arm_perf_collector(
        args.interval, args.last_secs, args.skip_last_secs
    )
    for line in arm_perf_collector.splitlines():
        key, value = line.split(",")
        if key in Map:
            db_fields[f"{Map[key]}"] = value
    res += arm_perf_collector
    intel_perfspect = read_intel_perfspect(
        args.interval, args.last_secs, args.skip_last_secs
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
        required=True,
        help="Name of CPU generation (e.g. cpl, milan, bergamo)",
    )
    parser.add_argument(
        "--interval", type=int, default=5, help="Metrics collection interval"
    )
    parser.add_argument(
        "--last-secs",
        type=int,
        default=300,
        help='Last N seconds of metrics to process as benchmarking stage. Recommended value: Taobench: 600; Feedsim: 300; Spark (full run): value of "execution_time_test_93586"; Spark (stage 2.0): value of "execution_time_test_93586-stage-2.0"; Video Transcode: value of "level6_time_secs"; Django: 300; MediaWiki: 600',
    )
    parser.add_argument(
        "--skip-last-secs",
        type=int,
        default=0,
        help="Skip the last N seconds of metrics. Recommended value: Taobench: 120; Feedsim: 30; Spark: 10; Video Transcode: 10; Django: 60; MediaWiki: 30",
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
