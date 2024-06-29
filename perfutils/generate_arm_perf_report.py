#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
import csv
import functools
import io
import itertools

import click
import pandas as pd
import tabulate


def skip_if_missing(f):
    @functools.wraps(f)
    def wrap(*args, **kwargs):
        try:
            return f(*args, **kwargs)
        except KeyError:
            pass

    return wrap


def read_csv(perf_csv_file):
    df = pd.read_csv(
        perf_csv_file,
        names=[
            "timestamp",
            "socket",
            "numcpus",
            "counter_value",
            "counter_unit",
            "event_name",
            "counter_runtime",
            "mux",
            "optional_metric_value",
            "optional_metric_unit",
            "1",
            "2",
        ],
    )
    return df


def aggregate_stats(derived_metric):
    derived_series = derived_metric["series"]
    prefix = derived_metric.get("prefix", 1.0)
    return {
        "min": derived_series.min() * prefix,
        "mean": derived_series.mean() * prefix,
        "std": derived_series.std() * prefix,
        "p95": derived_series.quantile(0.95) * prefix,
        "max": derived_series.max() * prefix,
    }


def render_as_csv(metrics, delimiter=","):
    output = io.StringIO()
    csv_writer = csv.writer(output, delimiter=delimiter)
    csv_writer.writerow(["metric", "mean", "stddev", "min", "p95", "max"])
    for metric in metrics:
        stats = aggregate_stats(metric)
        csv_writer.writerow(
            [
                metric["name"],
                stats["mean"],
                stats["std"],
                stats["min"],
                stats["p95"],
                stats["max"],
            ]
        )
    return output.getvalue()


def render_as_table(metrics):
    headers = ["Metric", "Mean", "StdDev", "Min", "P95", "Max"]
    table = []
    for metric in metrics:
        stats = aggregate_stats(metric)
        row = [
            metric["name"],
            round(stats["mean"], 4),
            round(stats["std"], 4),
            round(stats["min"], 4),
            round(stats["p95"], 4),
            round(stats["max"], 4),
        ]
        table.append(row)
    return tabulate.tabulate(
        table, headers, tablefmt="simple", stralign="left", numalign="right"
    )


def concat_series(metrics, shortest_length_series):
    short_series = shortest_length_series["series"]
    series = []
    for m in metrics:
        m["series"].index = short_series.index
        m["series"].name = m["name"]
        prefix = m.get("prefix", 1.0)
        series.append(m["series"] * prefix)
    return pd.concat(series, axis=1).reset_index()


@skip_if_missing
def timestamp(grouped_df):
    ts_series = grouped_df.get_group("cycles").timestamp
    return {"name": "Timestamp_Secs", "series": ts_series}


@skip_if_missing
def duration(grouped_df):
    duration_series = grouped_df.get_group("duration_time").counter_value
    mux_series = grouped_df.get_group("instructions").mux / 100.0

    duration_series.index = mux_series.index

    return {
        "name": "Per-Sample Effective Sampling Duration (msecs)",
        "series": duration_series * mux_series,
        "prefix": 10**-6,
    }


@skip_if_missing
def mips(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    return {"name": "MIPS", "series": inst_series, "prefix": 10**-6}


@skip_if_missing
def ipc(grouped_df):
    cycles_series = grouped_df.get_group("cycles").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    cycles_series.index = inst_series.index

    ipc_series = inst_series.div(cycles_series)
    return {"name": "IPC", "series": ipc_series}


@skip_if_missing
def flops(grouped_df):
    fp_scale_series = grouped_df.get_group("fp_scale_ops_spec").counter_value
    fp_fixed_series = grouped_df.get_group("fp_fixed_ops_spec").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    fp_scale_series.index = duration_series.index
    fp_fixed_series.index = duration_series.index

    flop_sum_series = fp_fixed_series + fp_scale_series
    flops_series = flop_sum_series.div(duration_series / 10**9) / 10**9
    return {
        "name": "GFLOPS (any precision, incl SVE)",
        "series": flops_series,
    }


@skip_if_missing
def sve_flops(grouped_df):
    fp_scale_series = grouped_df.get_group("fp_scale_ops_spec").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    fp_scale_series.index = duration_series.index

    sve_flops_series = fp_scale_series.div(duration_series / 10**9) / 10**9
    return {
        "name": "SVE GFLOPS (any precision)",
        "series": sve_flops_series,
    }


@skip_if_missing
def l1_icache_mpki(grouped_df):
    icache_refill_series = grouped_df.get_group("l1i_cache_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    icache_refill_series.index = instructions_series.index
    return {
        "name": "L1 ICache MPKI",
        "series": icache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def l1_dcache_mpki(grouped_df):
    dcache_refill_series = grouped_df.get_group("l1d_cache_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    dcache_refill_series.index = instructions_series.index
    return {
        "name": "L1 DCache MPKI",
        "series": dcache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def l2_cache_mpki(grouped_df):
    l2_dcache_refill_series = grouped_df.get_group("l2d_cache_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l2_dcache_refill_series.index = instructions_series.index
    return {
        "name": "L2 Cache MPKI",
        "series": l2_dcache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def l3_cache_mpki(grouped_df):
    l3_cache_miss_rd_series = grouped_df.get_group("ll_cache_miss_rd").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l3_cache_miss_rd_series.index = instructions_series.index
    return {
        "name": "L3 Cache MPKI",
        "series": l3_cache_miss_rd_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def itlb_mpki(grouped_df):
    l1i_tlb_refill_series = grouped_df.get_group("l1i_tlb_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l1i_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L1 iTLB MPKI",
        "series": l1i_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def dtlb_mpki(grouped_df):
    l1d_tlb_refill_series = grouped_df.get_group("l1d_tlb_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l1d_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L1 dTLB MPKI",
        "series": l1d_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def l2tlb_mpki(grouped_df):
    l2_tlb_refill_series = grouped_df.get_group("l2d_tlb_refill").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l2_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L2 TLB MPKI",
        "series": l2_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def itlb_walk_mpki(grouped_df):
    itlb_walk_series = grouped_df.get_group("itlb_walk").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    itlb_walk_series.index = instructions_series.index

    return {
        "name": "iTLB Walk MPKI",
        "series": itlb_walk_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def dtlb_walk_mpki(grouped_df):
    dtlb_walk_series = grouped_df.get_group("dtlb_walk").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    dtlb_walk_series.index = instructions_series.index

    return {
        "name": "dTLB Walk MPKI",
        "series": dtlb_walk_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def retiring_slots(grouped_df):
    op_retired_series = grouped_df.get_group("op_retired").counter_value
    op_spec_series = grouped_df.get_group("op_spec").counter_value
    stall_slot_series = grouped_df.get_group("stall_slot").counter_value
    cycles = grouped_df.get_group("cycles").counter_value

    op_retired_series.index = cycles.index
    op_spec_series.index = cycles.index
    stall_slot_series.index = cycles.index

    retiring_slots_series = (op_retired_series / op_spec_series) * (
        1 - (stall_slot_series / (8 * cycles))
    )
    return {
        "name": "TopDown Retiring %",
        "series": retiring_slots_series,
        "prefix": 100,
    }


@skip_if_missing
def frontend_bound_slots(grouped_df):
    stall_slot_fe_series = grouped_df.get_group("stall_slot_frontend").counter_value
    br_mis_pred_series = grouped_df.get_group("br_mis_pred").counter_value
    cycles_series = grouped_df.get_group("cycles").counter_value

    stall_slot_fe_series.index = cycles_series.index
    br_mis_pred_series.index = cycles_series.index

    fe_bound_series = (stall_slot_fe_series / (8 * cycles_series)) - (
        (br_mis_pred_series * 4) / cycles_series
    )
    return {
        "name": "TopDown FrontendBound %",
        "series": fe_bound_series,
        "prefix": 100,
    }


@skip_if_missing
def backend_bound_slots(grouped_df):
    stall_slot_be_series = grouped_df.get_group("stall_slot_backend").counter_value
    cycles_series = grouped_df.get_group("cycles").counter_value

    stall_slot_be_series.index = cycles_series.index

    be_bound_series = stall_slot_be_series / (8 * cycles_series)
    return {
        "name": "TopDown BackendBound %",
        "series": be_bound_series,
        "prefix": 100,
    }


@skip_if_missing
def nvidia_scf_mem_read_bw_MBps(grouped_df):
    cmem_rd_data_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_data/"
    ).counter_value
    duration_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_data/"
    ).counter_runtime

    cmem_rd_data_series.index = duration_series.index

    local_mem_read_series = cmem_rd_data_series * 32
    local_mem_bw_read_series = local_mem_read_series.div(duration_series)
    return {
        "name": "SFC Local Memory Read Bandwidth (MBps)",
        "series": local_mem_bw_read_series,
        "prefix": 1000,
    }


@skip_if_missing
def nvidia_scf_mem_write_bw_MBps(grouped_df):
    cmem_wr_bytes_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_wr_total_bytes/"
    ).counter_value
    duration_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_wr_total_bytes/"
    ).counter_runtime

    cmem_wr_bytes_series.index = duration_series.index

    local_mem_bw_write_series = cmem_wr_bytes_series.div(duration_series)
    return {
        "name": "SFC Local Memory Write Bandwidth (MBps)",
        "series": local_mem_bw_write_series,
        "prefix": 1000,
    }


@skip_if_missing
def nvidia_scf_mem_latency_ns(grouped_df):
    cmem_rd_outstanding_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_outstanding/"
    ).counter_value
    cmem_rd_access_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_access/"
    ).counter_value
    sfc_cycles_series = grouped_df.get_group("nvidia_scf_pmu_0/cycles/").counter_value
    duration_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_outstanding/"
    ).counter_runtime

    cmem_rd_outstanding_series.index = sfc_cycles_series.index
    cmem_rd_access_series.index = sfc_cycles_series.index
    duration_series.index = sfc_cycles_series.index

    local_mem_read_lat_ns_series = (
        cmem_rd_outstanding_series.div(cmem_rd_access_series)
    ) / (sfc_cycles_series.div(duration_series))
    return {
        "name": "SFC Local Memory Read Latency (nsecs)",
        "series": local_mem_read_lat_ns_series,
    }


@click.command()
@click.argument(
    "perf_csv_file", type=click.Path(exists=True, dir_okay=False, resolve_path=True)
)
@click.option(
    "-s",
    "--series",
    type=click.File(mode="w", lazy=True),
    help="Write derived time-series data as CSV into the designated file",
)
@click.option(
    "-f",
    "--format",
    type=click.Choice(["table", "csv"]),
    default="table",
    help="Output format",
)
def main(
    perf_csv_file: click.Path,
    series: click.File,
    format: click.Choice,
) -> None:
    df = read_csv(perf_csv_file)
    grouped_df = df.groupby("event_name")
    metrics = [
        timestamp(grouped_df),
        duration(grouped_df),
        mips(grouped_df),
        ipc(grouped_df),
        flops(grouped_df),
        sve_flops(grouped_df),
        retiring_slots(grouped_df),
        frontend_bound_slots(grouped_df),
        backend_bound_slots(grouped_df),
        l1_icache_mpki(grouped_df),
        l1_dcache_mpki(grouped_df),
        l2_cache_mpki(grouped_df),
        l3_cache_mpki(grouped_df),
        itlb_mpki(grouped_df),
        dtlb_mpki(grouped_df),
        l2tlb_mpki(grouped_df),
        itlb_walk_mpki(grouped_df),
        dtlb_walk_mpki(grouped_df),
        nvidia_scf_mem_read_bw_MBps(grouped_df),
        nvidia_scf_mem_write_bw_MBps(grouped_df),
        nvidia_scf_mem_latency_ns(grouped_df),
    ]

    filtered_metrics = list(itertools.filterfalse(lambda x: x is None, metrics))
    shortest_series = max(filtered_metrics, key=lambda m: m["series"].size)
    df_metrics = concat_series(filtered_metrics, shortest_series)
    if series:
        series.write(df_metrics.to_csv(index=False))
    if format == "table":
        output = render_as_table(filtered_metrics)
    else:  # format == "csv"
        output = render_as_csv(filtered_metrics)
    click.echo(output)


if __name__ == "__main__":
    main()
