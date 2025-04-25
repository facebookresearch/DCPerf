#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import csv
import functools
import io
import itertools
import subprocess
import typing

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


def read_csv(amd_perf_csv_file):
    df = pd.read_csv(
        amd_perf_csv_file,
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


def get_num_sockets(group):
    socket_series = group.socket
    return len(socket_series.reset_index().groupby("socket").index)


def get_duration_series(group):
    ts_series = group.timestamp
    num_sockets = get_num_sockets(group)
    prev_ts_series = pd.Series(
        [0.0] * num_sockets + list(ts_series.iloc[:-num_sockets])
    )
    prev_ts_series.index = ts_series.index
    return ts_series.sub(prev_ts_series)


@skip_if_missing
def timestamp(grouped_df):
    ts_series = grouped_df.get_group("cycles").timestamp
    return {"name": "Timestamp_Secs", "series": ts_series}


@skip_if_missing
def mips(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = get_duration_series(grouped_df.get_group("instructions"))
    return {
        "name": "Avg. MIPS (total)",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def ipc(grouped_df):
    cycles_series = grouped_df.get_group("cycles").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    cycles_series.index = inst_series.index

    ipc_series = inst_series.div(cycles_series)
    return {"name": "Avg. IPC", "series": ipc_series}


@skip_if_missing
def uops_per_instructions(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    uops = grouped_df.get_group("retired_uops").counter_value
    uops.index = inst_series.index
    upi_series = uops.div(inst_series)
    return {"name": "UPI", "series": upi_series}


@skip_if_missing
def uops_dispatched_opcache_per_instructions(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    uops = grouped_df.get_group("de_uops_dispatch_opcache").counter_value
    uops.index = inst_series.index
    uop_cache_series = uops.div(inst_series)
    return {"name": "OPCache UOPS per Instructions", "series": uop_cache_series}


@skip_if_missing
def uops_dispatched_decoder_per_instructions(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    uops = grouped_df.get_group("de_uops_dispatch_decoder").counter_value
    uops.index = inst_series.index
    uop_decoder_series = uops.div(inst_series)
    return {"name": "Decoder UOPS per Instructions", "series": uop_decoder_series}


@skip_if_missing
def microcoded_per_instructions(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    microcoded = grouped_df.get_group("retired_microcoded_instructions").counter_value
    microcoded.index = inst_series.index
    microcode_series = microcoded.div(inst_series)
    return {"name": "Microcoded Instruction Rate", "series": microcode_series}


@skip_if_missing
def frontend_stalls(grouped_df):
    stalled_cycles_any = grouped_df.get_group("stalled_cycles.any").counter_value
    stalled_cycles_back_pressure = grouped_df.get_group(
        "stalled_cycles.back_pressure"
    ).counter_value
    unhalted_cycles = grouped_df.get_group("cycles").counter_value
    unhalted_cycles.index = stalled_cycles_any.index
    stalled_cycles_back_pressure.index = stalled_cycles_any.index

    stalled_cycles_fe = stalled_cycles_any - stalled_cycles_back_pressure
    stalled_cycles_fe_series = stalled_cycles_fe.div(unhalted_cycles)
    return {
        "name": "Frontend Stalls",
        "series": stalled_cycles_fe_series,
        "prefix": 100,
    }


@skip_if_missing
def frontend_stalls_due_to_ic_miss(grouped_df):
    stalled_cycles_any = grouped_df.get_group("stalled_cycles.any").counter_value
    stalled_cycles_back_pressure = grouped_df.get_group(
        "stalled_cycles.back_pressure"
    ).counter_value
    stalled_cycles_idq_empty = grouped_df.get_group(
        "stalled_cycles.idq_empty"
    ).counter_value
    unhalted_cycles = grouped_df.get_group("cycles").counter_value
    unhalted_cycles.index = stalled_cycles_any.index
    stalled_cycles_back_pressure.index = stalled_cycles_any.index
    stalled_cycles_idq_empty.index = stalled_cycles_any.index

    stalled_cylces_ic_miss = (
        stalled_cycles_any - stalled_cycles_back_pressure - stalled_cycles_idq_empty
    )
    stalled_cycles_ic_series = stalled_cylces_ic_miss.div(unhalted_cycles)
    return {
        "name": "% Stalls due to IC Miss",
        "series": stalled_cycles_ic_series,
        "prefix": 100,
    }


@skip_if_missing
def backend_stalls(grouped_df):
    unhalted_cycles = grouped_df.get_group("cycles").counter_value
    stalled_cycles_back_pressure = grouped_df.get_group(
        "stalled_cycles.back_pressure"
    ).counter_value
    unhalted_cycles.index = stalled_cycles_back_pressure.index
    stalled_cycles_be_series = stalled_cycles_back_pressure.div(unhalted_cycles)
    return {"name": "Backend Stalls", "series": stalled_cycles_be_series, "prefix": 100}


@skip_if_missing
def branch_mispred_rate(grouped_df):
    retired_branches = grouped_df.get_group("retired_branch_instructions").counter_value
    retired_branches_mispred = grouped_df.get_group(
        "retired_branch_mispred"
    ).counter_value
    retired_branches.index = retired_branches_mispred.index
    branch_mispred_series = retired_branches_mispred.div(retired_branches)
    return {"name": "Branch Mispred %", "series": branch_mispred_series, "prefix": 100}


@skip_if_missing
def avg_mab_latency(grouped_df):
    mab_clks = grouped_df.get_group("mab_alloc_clks").counter_value
    pipe_allocs = grouped_df.get_group("mab_pipe_alloc").counter_value
    mab_clks.index = pipe_allocs.index
    mab_clks_series = mab_clks.div(pipe_allocs)
    return {"name": "Avg. MAB Latency (CCLKS)", "series": mab_clks_series}


@skip_if_missing
def l1_icache_mab_demand_requests_rate(grouped_df):
    mab_demand = grouped_df.get_group("ic_mab_requests_demand").counter_value
    mab_total = grouped_df.get_group("ic_mab_requests_total").counter_value
    mab_demand.index = mab_total.index
    mab_demand_series = mab_demand.div(mab_total)
    return {"name": "L1 ICache MAB Demand Request Rate", "series": mab_demand_series}


@skip_if_missing
def l1_icache_mab_prefetch_requests_rate(grouped_df):
    mab_prefetch = grouped_df.get_group("ic_mab_requests_prefetch").counter_value
    mab_total = grouped_df.get_group("ic_mab_requests_total").counter_value
    mab_prefetch.index = mab_total.index
    mab_prefetch_series = mab_prefetch.div(mab_total)
    return {
        "name": "L1 ICache MAB Prefetch Request Rate",
        "series": mab_prefetch_series,
    }


@skip_if_missing
def l1_icache_miss_rate(grouped_df):
    l1_ic_fetches = grouped_df.get_group("l1_ic_fetches").counter_value
    l1_ic_misses = grouped_df.get_group("l1_ic_misses").counter_value
    l1_ic_fetches.index = l1_ic_misses.index
    l1_ic_misses_pct_series = l1_ic_misses.div(l1_ic_fetches)
    return {
        "name": "L1 ICache Miss % (w/ prefetches)",
        "series": l1_ic_misses_pct_series,
        "prefix": 100,
    }


@skip_if_missing
def l1_icache_mpki(grouped_df):
    l1_ic_misses = grouped_df.get_group("l1_ic_misses").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    inst_series.index = l1_ic_misses.index
    l1_icache_mpki_series = l1_ic_misses.div(inst_series)
    return {
        "name": "L1 ICache MPKI (w/ prefetches)",
        "series": l1_icache_mpki_series,
        "prefix": 1000,
    }


@skip_if_missing
def l1_icache_fills_l2_ratio(grouped_df):
    fills_l2 = grouped_df.get_group("ic_cache_fill_l2").counter_value
    fills_sys = grouped_df.get_group("ic_cache_fill_sys").counter_value
    fills_l2.index = fills_sys.index
    l1_icache_fills_l2_ratio_series = fills_l2.div(fills_l2 + fills_sys)
    return {
        "name": "L1 ICache Fills L2 Ratio",
        "series": l1_icache_fills_l2_ratio_series,
    }


@skip_if_missing
def l1_icache_fills_sys_ratio(grouped_df):
    fills_l2 = grouped_df.get_group("ic_cache_fill_l2").counter_value
    fills_sys = grouped_df.get_group("ic_cache_fill_sys").counter_value
    fills_l2.index = fills_sys.index
    l1_icache_fills_sys_ratio_series = fills_sys.div(fills_l2 + fills_sys)
    return {
        "name": "L1 ICache Fills Sys Ratio",
        "series": l1_icache_fills_sys_ratio_series,
    }


@skip_if_missing
def l1_dcache_miss_rate(grouped_df):
    l1_dc_fetches = grouped_df.get_group("l1_dc_accesses").counter_value
    l1_dc_misses = grouped_df.get_group("l1_dc_misses").counter_value
    l1_dc_fetches.index = l1_dc_misses.index
    l1_dcache_miss_rate_series = l1_dc_misses.div(l1_dc_fetches)
    return {
        "name": "L1 DCache Miss % (w/ prefetches)",
        "series": l1_dcache_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def l1_dcache_mpki(grouped_df):
    l1_dc_misses = grouped_df.get_group("l1_dc_misses").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    inst_series.index = l1_dc_misses.index
    l1_dcache_mpki_series = l1_dc_misses.div(inst_series)
    return {
        "name": "L1 DCache MPKI (w/ prefetches)",
        "series": l1_dcache_mpki_series,
        "prefix": 1000,
    }


@skip_if_missing
def l2_code_miss_rate(grouped_df):
    l2_ic_requests_g1 = grouped_df.get_group("l2_ic_requests_g1").counter_value
    l2_ic_requests_g2 = grouped_df.get_group("l2_ic_requests_g2").counter_value
    l2_ic_hits = grouped_df.get_group("l2_ic_hits").counter_value

    l2_ic_requests_g1.index = l2_ic_hits.index
    l2_ic_requests_g2.index = l2_ic_hits.index

    l2_ic_accesses = l2_ic_requests_g1 + l2_ic_requests_g2
    l2_ic_misses = l2_ic_accesses - l2_ic_hits

    l2_code_miss_rate_series = l2_ic_misses.div(l2_ic_accesses)
    return {"name": "L2 Code Miss %", "series": l2_code_miss_rate_series, "prefix": 100}


@skip_if_missing
def l2_code_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_ic_requests_g1 = grouped_df.get_group("l2_ic_requests_g1").counter_value
    l2_ic_requests_g2 = grouped_df.get_group("l2_ic_requests_g2").counter_value
    l2_ic_hits = grouped_df.get_group("l2_ic_hits").counter_value

    inst_series.index = l2_ic_hits.index
    l2_ic_requests_g1.index = l2_ic_hits.index
    l2_ic_requests_g2.index = l2_ic_hits.index

    l2_ic_accesses = l2_ic_requests_g1 + l2_ic_requests_g2
    l2_ic_misses = l2_ic_accesses - l2_ic_hits
    l2_code_mpki_series = l2_ic_misses.div(inst_series)
    return {"name": "L2 Code MPKI", "series": l2_code_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_data_miss_rate(grouped_df):
    l2_dc_requests = grouped_df.get_group("l2_dc_requests").counter_value
    l2_dc_hits = grouped_df.get_group("l2_dc_hits").counter_value

    l2_dc_hits.index = l2_dc_requests.index
    l2_dc_misses = l2_dc_requests - l2_dc_hits
    l2_data_miss_rate_series = l2_dc_misses.div(l2_dc_requests)
    return {"name": "L2 Data Miss %", "series": l2_data_miss_rate_series, "prefix": 100}


@skip_if_missing
def l2_data_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_dc_requests = grouped_df.get_group("l2_dc_requests").counter_value
    l2_dc_hits = grouped_df.get_group("l2_dc_hits").counter_value

    inst_series.index = l2_dc_requests.index
    l2_dc_hits.index = l2_dc_requests.index
    l2_dc_misses = l2_dc_requests - l2_dc_hits
    l2_data_mpki_series = l2_dc_misses.div(inst_series)
    return {"name": "L2 Data MPKI", "series": l2_data_mpki_series, "prefix": 1000}


@skip_if_missing
def llc_miss_rate(grouped_df):
    llc_accesses = grouped_df.get_group("l3_acceses").counter_value
    llc_misses = grouped_df.get_group("l3_misses").counter_value
    llc_misses.index = llc_accesses.index
    llc_miss_rate_series = llc_misses.div(llc_accesses)
    return {"name": "LLC Miss %", "series": llc_miss_rate_series, "prefix": 100}


@skip_if_missing
def llc_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    llc_misses = grouped_df.get_group("l3_misses").counter_value
    llc_misses.index = inst_series.index
    llc_mpki_series = llc_misses.div(inst_series)
    return {"name": "LLC MPKI", "series": llc_mpki_series, "prefix": 1000}


@skip_if_missing
def llc_avg_load_to_use_lat_clks(grouped_df):
    l3_fill_rd_resp = grouped_df.get_group("l3_fill_rd_resp_lat").counter_value
    l3_rd_resp_cnt = grouped_df.get_group("l3_rd_resp_cnt").counter_value
    l3_fill_other = grouped_df.get_group("l3_fill_lat_other_rd_resp").counter_value

    l3_rd_resp_cnt.index = l3_fill_rd_resp.index
    l3_fill_other.index = l3_fill_rd_resp.index

    l3_total_fill_lat = l3_fill_rd_resp * 16
    l3_total_resp_cnt = l3_rd_resp_cnt + l3_fill_other

    llc_avg_load_to_use_lat_clks_series = l3_total_fill_lat.div(l3_total_resp_cnt)
    return {
        "name": "LLC Avg Load-to-Use Latency (CLKs)",
        "series": llc_avg_load_to_use_lat_clks_series,
    }


# TODO(cltorres): Refactor mpki calculations into its own function
@skip_if_missing
def itlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    itlb_misses = grouped_df.get_group("itlb_misses").counter_value
    itlb_misses.index = inst_series.index
    itlb_mpki_series = itlb_misses.div(inst_series)
    return {"name": "iTLB MPKI", "series": itlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_itlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_dtlb_misses = grouped_df.get_group("l2_itlb_misses").counter_value
    l2_dtlb_misses.index = inst_series.index
    l2_itlb_mpki_series = l2_dtlb_misses.div(inst_series)
    return {"name": "L2 iTLB MPKI", "series": l2_itlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_4k_itlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_4k_itlb_misses = grouped_df.get_group("l2_4k_itlb_misses").counter_value
    l2_4k_itlb_misses.index = inst_series.index
    l2_4k_itlb_mpki_series = l2_4k_itlb_misses.div(inst_series)
    return {"name": "L2 4K iTLB MPKI", "series": l2_4k_itlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_2m_itlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_2m_itlb_misses = grouped_df.get_group("l2_2m_itlb_misses").counter_value
    l2_2m_itlb_misses.index = inst_series.index
    l2_2m_itlb_mpki_series = l2_2m_itlb_misses.div(inst_series)
    return {"name": "L2 2M iTLB MPKI", "series": l2_2m_itlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_1g_itlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_1g_itlb_misses = grouped_df.get_group("l2_1g_itlb_misses").counter_value
    l2_1g_itlb_misses.index = inst_series.index
    l2_1g_itlb_mpki_series = l2_1g_itlb_misses.div(inst_series)
    return {"name": "L2 1G iTLB MPKI", "series": l2_1g_itlb_mpki_series, "prefix": 1000}


@skip_if_missing
def dtlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    dtlb_misses_series = grouped_df.get_group("dtlb_misses").counter_value
    dtlb_misses_series.index = inst_series.index
    dtlb_mpki_series = dtlb_misses_series.div(inst_series)
    return {"name": "dTLB MPKI", "series": dtlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l1_4k_dtlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_4k_dtlb_misses = grouped_df.get_group("l1_4k_dtlb_misses").counter_value
    l2_4k_dtlb_misses.index = inst_series.index
    l1_4k_dtlb_mpki_series = l2_4k_dtlb_misses.div(inst_series)
    return {"name": "L1 4K dTLB MPKI", "series": l1_4k_dtlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l1_2m_dtlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_2m_dtlb_misses = grouped_df.get_group("l1_2m_dtlb_misses").counter_value
    l2_2m_dtlb_misses.index = inst_series.index
    l1_2m_dtlb_mpki_series = l2_2m_dtlb_misses.div(inst_series)
    return {"name": "L1 2M dTLB MPKI", "series": l1_2m_dtlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l1_1g_dtlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_1g_dtlb_misses = grouped_df.get_group("l1_1g_dtlb_misses").counter_value
    l2_1g_dtlb_misses.index = inst_series.index
    l1_1g_dtlb_mpki_series = l2_1g_dtlb_misses.div(inst_series)
    return {"name": "L1 1G dTLB MPKI", "series": l1_1g_dtlb_mpki_series, "prefix": 1000}


@skip_if_missing
def l2_dtlb_mpki(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_dtlb_misses = grouped_df.get_group("l2_dtlb_misses").counter_value
    l2_dtlb_misses.index = inst_series.index
    l2_dtlb_mpki_series = l2_dtlb_misses.div(inst_series)
    return {"name": "L2 dTLB MPKI", "series": l2_dtlb_mpki_series, "prefix": 1000}


@skip_if_missing
def mem_read_bw_MBps(grouped_df):
    umc_c_read_requests = grouped_df.get_group("umc_c_read_requests").counter_value
    umc_g_read_requests = grouped_df.get_group("umc_g_read_requests").counter_value
    umc_c_cancels_issued = grouped_df.get_group("umc_c_cancels_issued").counter_value
    umc_g_cancels_issued = grouped_df.get_group("umc_g_cancels_issued").counter_value
    duration_series = get_duration_series(grouped_df.get_group("umc_c_read_requests"))

    umc_g_read_requests.index = umc_c_read_requests.index
    umc_c_cancels_issued.index = umc_c_read_requests.index
    umc_g_cancels_issued.index = umc_c_read_requests.index

    umc_c_read_traffic = (umc_c_read_requests - umc_c_cancels_issued) * 2 * 64
    umc_g_read_traffic = (umc_g_read_requests - umc_g_cancels_issued) * 2 * 64
    total_umc_reads = umc_c_read_traffic + umc_g_read_traffic
    return {
        "name": "Total Memory Read BW (MB/s)",
        "series": total_umc_reads.div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def mem_write_bw_MBps(grouped_df):
    umc_c_write_requests = grouped_df.get_group("umc_c_write_requests").counter_value
    umc_d_write_requests = grouped_df.get_group("umc_d_write_requests").counter_value
    umc_g_write_requests = grouped_df.get_group("umc_g_write_requests").counter_value
    umc_h_write_requests = grouped_df.get_group("umc_h_write_requests").counter_value
    duration_series = get_duration_series(grouped_df.get_group("umc_c_write_requests"))

    umc_d_write_requests.index = umc_c_write_requests.index
    umc_g_write_requests.index = umc_c_write_requests.index
    umc_h_write_requests.index = umc_c_write_requests.index

    total_umc_writes = 64 * (
        umc_c_write_requests
        + umc_d_write_requests
        + umc_g_write_requests
        + umc_h_write_requests
    )
    return {
        "name": "Total Memory Write BW (MB/s)",
        "series": total_umc_writes.div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def zen4_mem_read_bw_MBps(grouped_df):
    umc_a_read_requests = grouped_df.get_group("umc_a_read_requests").counter_value
    umc_b_read_requests = grouped_df.get_group("umc_b_read_requests").counter_value
    umc_c_read_requests = grouped_df.get_group("umc_c_read_requests").counter_value
    umc_d_read_requests = grouped_df.get_group("umc_d_read_requests").counter_value
    umc_e_read_requests = grouped_df.get_group("umc_e_read_requests").counter_value
    umc_f_read_requests = grouped_df.get_group("umc_f_read_requests").counter_value
    umc_g_read_requests = grouped_df.get_group("umc_g_read_requests").counter_value
    umc_h_read_requests = grouped_df.get_group("umc_h_read_requests").counter_value
    umc_i_read_requests = grouped_df.get_group("umc_i_read_requests").counter_value
    umc_j_read_requests = grouped_df.get_group("umc_j_read_requests").counter_value
    umc_k_read_requests = grouped_df.get_group("umc_k_read_requests").counter_value
    umc_l_read_requests = grouped_df.get_group("umc_l_read_requests").counter_value
    duration_series = get_duration_series(grouped_df.get_group("umc_a_read_requests"))

    umc_b_read_requests.index = umc_a_read_requests.index
    umc_c_read_requests.index = umc_a_read_requests.index
    umc_d_read_requests.index = umc_a_read_requests.index
    umc_e_read_requests.index = umc_a_read_requests.index
    umc_f_read_requests.index = umc_a_read_requests.index
    umc_g_read_requests.index = umc_a_read_requests.index
    umc_h_read_requests.index = umc_a_read_requests.index
    umc_i_read_requests.index = umc_a_read_requests.index
    umc_j_read_requests.index = umc_a_read_requests.index
    umc_k_read_requests.index = umc_a_read_requests.index
    umc_l_read_requests.index = umc_a_read_requests.index

    total_umc_reads = 64 * (
        umc_a_read_requests
        + umc_b_read_requests
        + umc_c_read_requests
        + umc_d_read_requests
        + umc_e_read_requests
        + umc_f_read_requests
        + umc_g_read_requests
        + umc_h_read_requests
        + umc_i_read_requests
        + umc_j_read_requests
        + umc_k_read_requests
        + umc_l_read_requests
    )

    return {
        "name": "Total Memory Read BW (MB/s)",
        "series": total_umc_reads.div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def zen4_mem_write_bw_MBps(grouped_df):
    umc_a_write_requests = grouped_df.get_group("umc_a_write_requests").counter_value
    umc_b_write_requests = grouped_df.get_group("umc_b_write_requests").counter_value
    umc_c_write_requests = grouped_df.get_group("umc_c_write_requests").counter_value
    umc_d_write_requests = grouped_df.get_group("umc_d_write_requests").counter_value
    umc_e_write_requests = grouped_df.get_group("umc_e_write_requests").counter_value
    umc_f_write_requests = grouped_df.get_group("umc_f_write_requests").counter_value
    umc_g_write_requests = grouped_df.get_group("umc_g_write_requests").counter_value
    umc_h_write_requests = grouped_df.get_group("umc_h_write_requests").counter_value
    umc_i_write_requests = grouped_df.get_group("umc_i_write_requests").counter_value
    umc_j_write_requests = grouped_df.get_group("umc_j_write_requests").counter_value
    umc_k_write_requests = grouped_df.get_group("umc_k_write_requests").counter_value
    umc_l_write_requests = grouped_df.get_group("umc_l_write_requests").counter_value
    duration_series = get_duration_series(grouped_df.get_group("umc_a_write_requests"))

    umc_b_write_requests.index = umc_a_write_requests.index
    umc_c_write_requests.index = umc_a_write_requests.index
    umc_d_write_requests.index = umc_a_write_requests.index
    umc_e_write_requests.index = umc_a_write_requests.index
    umc_f_write_requests.index = umc_a_write_requests.index
    umc_g_write_requests.index = umc_a_write_requests.index
    umc_h_write_requests.index = umc_a_write_requests.index
    umc_i_write_requests.index = umc_a_write_requests.index
    umc_j_write_requests.index = umc_a_write_requests.index
    umc_k_write_requests.index = umc_a_write_requests.index
    umc_l_write_requests.index = umc_a_write_requests.index

    total_umc_writes = 64 * (
        umc_a_write_requests
        + umc_b_write_requests
        + umc_c_write_requests
        + umc_d_write_requests
        + umc_e_write_requests
        + umc_f_write_requests
        + umc_g_write_requests
        + umc_h_write_requests
        + umc_i_write_requests
        + umc_j_write_requests
        + umc_k_write_requests
        + umc_l_write_requests
    )

    return {
        "name": "Total Memory Write BW (MB/s)",
        "series": total_umc_writes.div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def zen5_user_instr_pct(grouped_df):
    user_instr_series = grouped_df.get_group("instructions:u").counter_value
    instr_series = grouped_df.get_group("instructions").counter_value
    user_instr_series.index = instr_series.index
    user_instr_pct_series = user_instr_series.div(instr_series) * 100
    return {"name": "User Instr %", "series": user_instr_pct_series}


@skip_if_missing
def zen5_kernel_instr_pct(grouped_df):
    kernel_instr_series = grouped_df.get_group("instructions:kh").counter_value
    instr_series = grouped_df.get_group("instructions").counter_value
    kernel_instr_series.index = instr_series.index
    kernel_instr_pct_series = kernel_instr_series.div(instr_series) * 100
    return {"name": "Kernel Instr %", "series": kernel_instr_pct_series}


@skip_if_missing
def zen5_overall_utilization_pct(grouped_df):
    mperf_series = grouped_df.get_group("mperf").counter_value
    tsc_series = grouped_df.get_group("tsc").counter_value
    mperf_series.index = tsc_series.index
    overall_utilization_pct_series = mperf_series.div(tsc_series) * 100
    return {"name": "Overall Utilization %", "series": overall_utilization_pct_series}


@skip_if_missing
def zen5_ops_per_instruction(grouped_df):
    ex_ret_ops_series = grouped_df.get_group("ex_ret_ops").counter_value
    instr_series = grouped_df.get_group("instructions").counter_value
    ex_ret_ops_series.index = instr_series.index
    ops_per_instr_series = ex_ret_ops_series.div(instr_series)
    return {"name": "Ops per Instructions", "series": ops_per_instr_series}


@skip_if_missing
def zen5_dispatched_ops_per_cycle(grouped_df):
    de_dis_ops_series = grouped_df.get_group(
        "de_dis_ops_from_decoder.any_fp_dispatch+de_dis_ops_from_decoder.any_integer_dispatch"
    ).counter_value
    aperf_series = grouped_df.get_group("cycles").counter_value
    de_dis_ops_series.index = aperf_series.index
    dispatched_ops_per_cycle_series = de_dis_ops_series.div(aperf_series)
    return {
        "name": "Dispatched Ops per Cycle",
        "series": dispatched_ops_per_cycle_series,
    }


@skip_if_missing
def zen5_dispatched_ops_per_cycle_v2(grouped_df):
    de_dis_ops_series = grouped_df.get_group(
        "de_dis_ops_from_decoder.any_fp_dispatch+de_dis_ops_from_decoder.disp_op_type.any_integer_dispatch"
    ).counter_value
    aperf_series = grouped_df.get_group("cycles").counter_value
    de_dis_ops_series.index = aperf_series.index
    dispatched_ops_per_cycle_series = de_dis_ops_series.div(aperf_series)
    return {
        "name": "Dispatched Ops per Cycle",
        "series": dispatched_ops_per_cycle_series,
    }


@skip_if_missing
def zen5_microcoded_pki(grouped_df):
    ex_ret_ucode_instr_series = grouped_df.get_group("ex_ret_ucode_instr").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_ucode_instr_series.index = inst_series.index
    ex_ret_ucode_instr_pki_series = ex_ret_ucode_instr_series.div(inst_series)
    return {
        "name": "Microcoded PKI",
        "series": ex_ret_ucode_instr_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_microcoded_uops_pct(grouped_df):
    ex_ret_ucode_ops_series = grouped_df.get_group("ex_ret_ucode_ops").counter_value
    ex_ret_ops_ucpercent_series = grouped_df.get_group(
        "ex_ret_ops_ucpercent"
    ).counter_value
    ex_ret_ucode_ops_series.index = ex_ret_ops_ucpercent_series.index
    microcoded_uops_pct_series = (
        ex_ret_ucode_ops_series.div(ex_ret_ops_ucpercent_series) * 100
    )
    return {
        "name": "Microcoded uops % of all uops",
        "series": microcoded_uops_pct_series,
    }


@skip_if_missing
def zen5_interrupts_pki(grouped_df):
    ls_int_taken_series = grouped_df.get_group("ls_int_taken").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_int_taken_series.index = inst_series.index
    ls_int_taken_pki_series = ls_int_taken_series.div(inst_series)
    return {"name": "Interrupts PKI", "series": ls_int_taken_pki_series, "prefix": 1000}


@skip_if_missing
def zen5_opcache_ops_pki(grouped_df):
    de_src_op_disp_op_cache_series = grouped_df.get_group(
        "de_src_op_disp.op_cache"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    de_src_op_disp_op_cache_series.index = inst_series.index
    de_src_op_disp_op_cache_pki_series = de_src_op_disp_op_cache_series.div(inst_series)
    return {
        "name": "OPCache Ops PKI",
        "series": de_src_op_disp_op_cache_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_decoder_ops_pki(grouped_df):
    de_src_op_disp_decoder_series = grouped_df.get_group(
        "de_src_op_disp.decoder"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    de_src_op_disp_decoder_series.index = inst_series.index
    de_src_op_disp_decoder_pki_series = de_src_op_disp_decoder_series.div(inst_series)
    return {
        "name": "Decoder Ops PKI",
        "series": de_src_op_disp_decoder_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_decoder_ops_pki_v2(grouped_df):
    de_src_op_disp_x86_decoder_series = grouped_df.get_group(
        "de_src_op_disp.x86_decoder"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    de_src_op_disp_x86_decoder_series.index = inst_series.index
    de_src_op_disp_x86_decoder_pki_series = de_src_op_disp_x86_decoder_series.div(
        inst_series
    )
    return {
        "name": "Decoder Ops PKI",
        "series": de_src_op_disp_x86_decoder_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_frontend_bound_pct(grouped_df):
    de_no_dispatch_per_slot_no_ops_from_frontend_series = grouped_df.get_group(
        "de_no_dispatch_per_slot.no_ops_from_frontend"
    ).counter_value
    ls_not_halted_cyc_series = grouped_df.get_group("ls_not_halted_cyc").counter_value
    de_no_dispatch_per_slot_no_ops_from_frontend_series.index = (
        ls_not_halted_cyc_series.index
    )
    frontend_bound_pct_series = (
        de_no_dispatch_per_slot_no_ops_from_frontend_series.div(
            ls_not_halted_cyc_series
        )
        * 100
        / 8
    )
    return {"name": "Frontend Bound %", "series": frontend_bound_pct_series}


@skip_if_missing
def zen5_frontend_bound_by_latency_pct(grouped_df):
    frontend_latency_series = grouped_df.get_group("frontend_latency").counter_value
    ls_not_halted_cyc_series = grouped_df.get_group("ls_not_halted_cyc").counter_value
    frontend_latency_series.index = ls_not_halted_cyc_series.index
    frontend_bound_by_latency_pct_series = (
        frontend_latency_series.div(ls_not_halted_cyc_series) * 100
    )
    return {
        "name": "Frontend Bound by Latency %",
        "series": frontend_bound_by_latency_pct_series,
    }


@skip_if_missing
def zen5_frontend_bound_by_bandwidth_pct(
    frontend_bound_metrics, frontend_bound_by_latency_metrics
):
    frontend_bound_pct_series = frontend_bound_metrics["Frontend Bound %"]
    frontend_bound_by_latency_pct_series = frontend_bound_by_latency_metrics[
        "Frontend Bound by Latency %"
    ]
    frontend_bound_by_bandwidth_pct_series = (
        frontend_bound_pct_series - frontend_bound_by_latency_pct_series
    )
    return {
        "name": "Frontend Bound by Bandwidth %",
        "series": frontend_bound_by_bandwidth_pct_series,
    }


@skip_if_missing
def zen5_backend_bound_pct(grouped_df):
    de_no_dispatch_per_slot_backend_stalls_series = grouped_df.get_group(
        "de_no_dispatch_per_slot.backend_stalls"
    ).counter_value
    ls_not_halted_cyc_backend_series = grouped_df.get_group(
        "ls_not_halted_cyc_backend"
    ).counter_value
    de_no_dispatch_per_slot_backend_stalls_series.index = (
        ls_not_halted_cyc_backend_series.index
    )
    backend_bound_pct_series = (
        de_no_dispatch_per_slot_backend_stalls_series.div(
            ls_not_halted_cyc_backend_series
        )
        * 100
        / 8
    )
    return {"name": "Backend Bound %", "series": backend_bound_pct_series}


@skip_if_missing
def zen5_backend_bound_by_memory_pct(grouped_df):
    de_no_dispatch_per_slot_backend_stalls_series = grouped_df.get_group(
        "de_no_dispatch_per_slot.backend_stalls"
    ).counter_value
    ex_no_retire_load_not_complete_series = grouped_df.get_group(
        "ex_no_retire.load_not_complete"
    ).counter_value
    ls_not_halted_cyc_backend_series = grouped_df.get_group(
        "ls_not_halted_cyc_backend"
    ).counter_value
    ex_no_retire_not_complete_series = grouped_df.get_group(
        "ex_no_retire.not_complete"
    ).counter_value
    de_no_dispatch_per_slot_backend_stalls_series.index = (
        ls_not_halted_cyc_backend_series.index
    )
    ex_no_retire_load_not_complete_series.index = ls_not_halted_cyc_backend_series.index
    ex_no_retire_not_complete_series.index = ls_not_halted_cyc_backend_series.index
    backend_bound_by_memory_pct_series = (
        (
            de_no_dispatch_per_slot_backend_stalls_series
            * ex_no_retire_load_not_complete_series
        ).div(ls_not_halted_cyc_backend_series * ex_no_retire_not_complete_series)
        * 100
        / 8
    )
    return {
        "name": "Backend Bound by Memory %",
        "series": backend_bound_by_memory_pct_series,
    }


@skip_if_missing
def zen5_backend_bound_by_cpu_pct(
    backend_bound_metrics, backend_bound_by_memory_metrics
):
    backend_bound_pct_series = backend_bound_metrics["Backend Bound %"]
    backend_bound_by_memory_pct_series = backend_bound_by_memory_metrics[
        "Backend Bound by Memory %"
    ]
    backend_bound_by_cpu_pct_series = (
        backend_bound_pct_series - backend_bound_by_memory_pct_series
    )
    return {"name": "Backend Bound by CPU %", "series": backend_bound_by_cpu_pct_series}


@skip_if_missing
def zen5_bad_speculation_pct(grouped_df):
    de_src_op_disp_all_series = grouped_df.get_group("de_src_op_disp.all").counter_value
    ex_ret_ops_series = grouped_df.get_group("ex_ret_ops").counter_value
    ls_not_halted_cyc_series = grouped_df.get_group("ls_not_halted_cyc").counter_value
    de_src_op_disp_all_series.index = ls_not_halted_cyc_series.index
    ex_ret_ops_series.index = ls_not_halted_cyc_series.index
    bad_speculation_pct_series = (
        (de_src_op_disp_all_series - ex_ret_ops_series).div(ls_not_halted_cyc_series)
        * 100
        / 8
    )
    return {"name": "Bad Speculation %", "series": bad_speculation_pct_series}


@skip_if_missing
def zen5_retiring_pct(grouped_df):
    ex_ret_ops_series = grouped_df.get_group("ex_ret_ops").counter_value
    ls_not_halted_cyc_series = grouped_df.get_group("ls_not_halted_cyc").counter_value
    ex_ret_ops_series.index = ls_not_halted_cyc_series.index
    retiring_pct_series = ex_ret_ops_series.div(ls_not_halted_cyc_series) * 100 / 8
    return {"name": "Retiring %", "series": retiring_pct_series}


@skip_if_missing
def zen5_smt_contention_pct(grouped_df):
    de_no_dispatch_per_slot_smt_contention_series = grouped_df.get_group(
        "de_no_dispatch_per_slot.smt_contention"
    ).counter_value
    ls_not_halted_cyc_backend_series = grouped_df.get_group(
        "ls_not_halted_cyc_backend"
    ).counter_value
    de_no_dispatch_per_slot_smt_contention_series.index = (
        ls_not_halted_cyc_backend_series.index
    )
    smt_contention_pct_series = (
        de_no_dispatch_per_slot_smt_contention_series.div(
            ls_not_halted_cyc_backend_series
        )
        * 100
        / 8
    )
    return {"name": "SMT Contention %", "series": smt_contention_pct_series}


@skip_if_missing
def zen5_op_queue_empty_pki(grouped_df):
    de_op_queue_empty_series = grouped_df.get_group("de_op_queue_empty").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    de_op_queue_empty_series.index = inst_series.index
    de_op_queue_empty_pki_series = de_op_queue_empty_series.div(inst_series)
    return {
        "name": "Op Queue Empty PKI",
        "series": de_op_queue_empty_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_token_stall_pki(grouped_df):
    de_dispatch_stall_cycle_dynamic_tokens_part1_all_series = grouped_df.get_group(
        "de_dispatch_stall_cycle_dynamic_tokens_part1.all"
    ).counter_value
    de_dispatch_stall_cycle_dynamic_tokens_part2_all_series = grouped_df.get_group(
        "de_dispatch_stall_cycle_dynamic_tokens_part2.all"
    ).counter_value
    de_dispatch_stall_cycle_dynamic_tokens_part1_all_series.index = (
        de_dispatch_stall_cycle_dynamic_tokens_part2_all_series.index
    )
    token_stall_series = (
        de_dispatch_stall_cycle_dynamic_tokens_part1_all_series
        + de_dispatch_stall_cycle_dynamic_tokens_part2_all_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    token_stall_series.index = inst_series.index
    token_stall_pki_series = token_stall_series.div(inst_series)
    return {"name": "Token Stall PKI", "series": token_stall_pki_series, "prefix": 1000}


@skip_if_missing
def zen5_branch_retired_pki(grouped_df):
    ex_ret_brn_series = grouped_df.get_group("ex_ret_brn").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_brn_series.index = inst_series.index
    ex_ret_brn_pki_series = ex_ret_brn_series.div(inst_series)
    return {
        "name": "Branch Retired PKI",
        "series": ex_ret_brn_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_mispred_pki(grouped_df):
    ex_ret_brn_misp_series = grouped_df.get_group("ex_ret_brn_misp").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_brn_misp_series.index = inst_series.index
    ex_ret_brn_misp_pki_series = ex_ret_brn_misp_series.div(inst_series)
    return {
        "name": "Branch Retired Mispred PKI",
        "series": ex_ret_brn_misp_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_taken_pki(grouped_df):
    ex_ret_brn_tkn_series = grouped_df.get_group("ex_ret_brn_tkn").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_brn_tkn_series.index = inst_series.index
    ex_ret_brn_tkn_pki_series = ex_ret_brn_tkn_series.div(inst_series)
    return {
        "name": "Branch Retired Taken PKI",
        "series": ex_ret_brn_tkn_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_indirect_mispred_pki(grouped_df):
    ex_ret_brn_ind_misp_series = grouped_df.get_group(
        "ex_ret_brn_ind_misp"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_brn_ind_misp_series.index = inst_series.index
    ex_ret_brn_ind_misp_pki_series = ex_ret_brn_ind_misp_series.div(inst_series)
    return {
        "name": "Branch Retired Indirect Mispred PKI",
        "series": ex_ret_brn_ind_misp_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_conditional_pki(grouped_df):
    ex_ret_cond_series = grouped_df.get_group("ex_ret_cond").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_cond_series.index = inst_series.index
    ex_ret_cond_pki_series = ex_ret_cond_series.div(inst_series)
    return {
        "name": "Branch Retired Conditional PKI",
        "series": ex_ret_cond_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_conditional_mispred_pki(grouped_df):
    ex_ret_cond_misp_series = grouped_df.get_group("ex_ret_cond_misp").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_cond_misp_series.index = inst_series.index
    ex_ret_cond_misp_pki_series = ex_ret_cond_misp_series.div(inst_series)
    return {
        "name": "Branch Retired Conditional Mispred PKI",
        "series": ex_ret_cond_misp_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_direct_jump_call_pki(grouped_df):
    ex_ret_uncond_brnch_instr_series = grouped_df.get_group(
        "ex_ret_uncond_brnch_instr"
    ).counter_value
    ex_ret_near_ret_series = grouped_df.get_group("ex_ret_near_ret").counter_value
    ex_ret_uncond_brnch_instr_series.index = ex_ret_near_ret_series.index
    direct_jump_call_series = ex_ret_uncond_brnch_instr_series - ex_ret_near_ret_series
    inst_series = grouped_df.get_group("instructions").counter_value
    direct_jump_call_series.index = inst_series.index
    direct_jump_call_pki_series = direct_jump_call_series.div(inst_series)
    return {
        "name": "Branch Retired Direct Jump/Call PKI",
        "series": direct_jump_call_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_indirect_jump_pki(grouped_df):
    ex_ret_ind_brch_instr_series = grouped_df.get_group(
        "ex_ret_ind_brch_instr"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_ind_brch_instr_series.index = inst_series.index
    ex_ret_ind_brch_instr_pki_series = ex_ret_ind_brch_instr_series.div(inst_series)
    return {
        "name": "Branch Retired Indirect Jump PKI",
        "series": ex_ret_ind_brch_instr_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_near_return_pki(grouped_df):
    ex_ret_near_ret_series = grouped_df.get_group("ex_ret_near_ret").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_near_ret_series.index = inst_series.index
    ex_ret_near_ret_pki_series = ex_ret_near_ret_series.div(inst_series)
    return {
        "name": "Branch Retired Near Return PKI",
        "series": ex_ret_near_ret_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_branch_retired_near_return_mispred_pki(grouped_df):
    ex_ret_near_ret_mispred_series = grouped_df.get_group(
        "ex_ret_near_ret_mispred"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_near_ret_mispred_series.index = inst_series.index
    ex_ret_near_ret_mispred_pki_series = ex_ret_near_ret_mispred_series.div(inst_series)
    return {
        "name": "Branch Retired Near Return Mispred PKI",
        "series": ex_ret_near_ret_mispred_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_fp_instr_retired_pki(grouped_df):
    ex_ret_mmx_fp_instr_all_series = grouped_df.get_group(
        "ex_ret_mmx_fp_instr.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_mmx_fp_instr_all_series.index = inst_series.index
    ex_ret_mmx_fp_instr_all_pki_series = ex_ret_mmx_fp_instr_all_series.div(inst_series)
    return {
        "name": "FP Instr Retired PKI",
        "series": ex_ret_mmx_fp_instr_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_fp_sse_avx_instr_retired_pki(grouped_df):
    ex_ret_mmx_fp_instr_sse_series = grouped_df.get_group(
        "ex_ret_mmx_fp_instr.sse"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ex_ret_mmx_fp_instr_sse_series.index = inst_series.index
    ex_ret_mmx_fp_instr_sse_pki_series = ex_ret_mmx_fp_instr_sse_series.div(inst_series)
    return {
        "name": "FP SSE AVX Instr Retired PKI",
        "series": ex_ret_mmx_fp_instr_sse_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_ls_uop_disp_ld_pki(grouped_df):
    ls_dispatch_ld_dispatch_series = grouped_df.get_group(
        "ls_dispatch.ld_dispatch"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_dispatch_ld_dispatch_series.index = inst_series.index
    ls_dispatch_ld_dispatch_pki_series = ls_dispatch_ld_dispatch_series.div(inst_series)
    return {
        "name": "LS uop disp Ld",
        "series": ls_dispatch_ld_dispatch_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_ls_uop_disp_st_pki(grouped_df):
    ls_dispatch_store_dispatch_series = grouped_df.get_group(
        "ls_dispatch.store_dispatch"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_dispatch_store_dispatch_series.index = inst_series.index
    ls_dispatch_store_dispatch_pki_series = ls_dispatch_store_dispatch_series.div(
        inst_series
    )
    return {
        "name": "LS uop disp St",
        "series": ls_dispatch_store_dispatch_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_os_locks_pki(grouped_df):
    r1f25_kh_series = grouped_df.get_group("r1f25:kh").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    r1f25_kh_series.index = inst_series.index
    r1f25_kh_pki_series = r1f25_kh_series.div(inst_series)
    return {"name": "OS Locks PKI", "series": r1f25_kh_pki_series, "prefix": 1000}


@skip_if_missing
def zen5_user_locks_pki(grouped_df):
    r1f25_u_series = grouped_df.get_group("r1f25:u").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    r1f25_u_series.index = inst_series.index
    r1f25_u_pki_series = r1f25_u_series.div(inst_series)
    return {"name": "User Locks PKI", "series": r1f25_u_pki_series, "prefix": 1000}


@skip_if_missing
def zen5_l1_icache_miss_pct(grouped_df):
    l1_ic_misses_series = grouped_df.get_group("l1_ic_misses").counter_value
    l1_ic_fetches_series = grouped_df.get_group("l1_ic_fetches").counter_value
    l1_ic_misses_series.index = l1_ic_fetches_series.index
    l1_icache_miss_pct_series = l1_ic_misses_series.div(l1_ic_fetches_series) * 100
    return {"name": "L1 ICache Miss %", "series": l1_icache_miss_pct_series}


@skip_if_missing
def zen5_any_l1_ic_fills_pki(grouped_df):
    ic_any_fills_from_sys_all_series = grouped_df.get_group(
        "ic_any_fills_from_sys.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_all_series.index = inst_series.index
    ic_any_fills_from_sys_all_pki_series = ic_any_fills_from_sys_all_series.div(
        inst_series
    )
    return {
        "name": "Any L1 IC Fills PKI",
        "series": ic_any_fills_from_sys_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_l2_pki(grouped_df):
    ic_any_fills_from_sys_local_l2_series = grouped_df.get_group(
        "ic_any_fills_from_sys.local_l2"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_local_l2_series.index = inst_series.index
    ic_any_fills_from_sys_local_l2_pki_series = (
        ic_any_fills_from_sys_local_l2_series.div(inst_series)
    )
    return {
        "name": "Any L1 IC Fills from L2 PKI",
        "series": ic_any_fills_from_sys_local_l2_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_l3_or_different_l2_in_same_ccx_pki(grouped_df):
    ic_any_fills_from_sys_local_ccx_series = grouped_df.get_group(
        "ic_any_fills_from_sys.local_ccx"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_local_ccx_series.index = inst_series.index
    ic_any_fills_from_sys_local_ccx_pki_series = (
        ic_any_fills_from_sys_local_ccx_series.div(inst_series)
    )
    return {
        "name": "Any L1 IC Fills from L3 or different L2 in same CCX PKI",
        "series": ic_any_fills_from_sys_local_ccx_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_dram_pki(grouped_df):
    ic_any_fills_from_sys_dram_io_series = grouped_df.get_group(
        "ic_any_fills_from_sys.dram_io"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_dram_io_series.index = inst_series.index
    ic_any_fills_from_sys_dram_io_pki_series = ic_any_fills_from_sys_dram_io_series.div(
        inst_series
    )
    return {
        "name": "Any L1 IC Fills from DRAM PKI",
        "series": ic_any_fills_from_sys_dram_io_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_other_ccx_pki(grouped_df):
    ic_any_fills_from_sys_remote_cache_series = grouped_df.get_group(
        "ic_any_fills_from_sys.remote_cache"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_remote_cache_series.index = inst_series.index
    ic_any_fills_from_sys_remote_cache_pki_series = (
        ic_any_fills_from_sys_remote_cache_series.div(inst_series)
    )
    return {
        "name": "Any L1 IC Fills from Other CCX PKI",
        "series": ic_any_fills_from_sys_remote_cache_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_l2_miss_pct(grouped_df):
    ic_any_fills_from_sys_local_l2_miss_series = grouped_df.get_group(
        "ic_any_fills_from_sys.local_l2_miss"
    ).counter_value
    ic_any_fills_from_sys_all_series = grouped_df.get_group(
        "ic_any_fills_from_sys.all"
    ).counter_value
    ic_any_fills_from_sys_local_l2_miss_series.index = (
        ic_any_fills_from_sys_all_series.index
    )
    any_l1_ic_fills_from_l2_miss_pct_series = (
        ic_any_fills_from_sys_local_l2_miss_series.div(ic_any_fills_from_sys_all_series)
        * 100
    )
    return {
        "name": "Any L1 IC Fills from L2 Miss %",
        "series": any_l1_ic_fills_from_l2_miss_pct_series,
    }


@skip_if_missing
def zen5_any_l1_ic_fills_from_l2_miss_pki(grouped_df):
    ic_any_fills_from_sys_local_l2_miss_series = grouped_df.get_group(
        "ic_any_fills_from_sys.local_l2_miss"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ic_any_fills_from_sys_local_l2_miss_series.index = inst_series.index
    ic_any_fills_from_sys_local_l2_miss_pki_series = (
        ic_any_fills_from_sys_local_l2_miss_series.div(inst_series)
    )
    return {
        "name": "Any L1 IC Fills from L2 Miss PKI",
        "series": ic_any_fills_from_sys_local_l2_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_dc_fills_pki(grouped_df):
    ls_any_fills_from_sys_all_series = grouped_df.get_group(
        "ls_any_fills_from_sys.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_any_fills_from_sys_all_series.index = inst_series.index
    ls_any_fills_from_sys_all_pki_series = ls_any_fills_from_sys_all_series.div(
        inst_series
    )
    return {
        "name": "Any L1 DC Fills PKI",
        "series": ls_any_fills_from_sys_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_dc_fills_from_l2_pki(grouped_df):
    ls_any_fills_from_sys_local_l2_series = grouped_df.get_group(
        "ls_any_fills_from_sys.local_l2"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_any_fills_from_sys_local_l2_series.index = inst_series.index
    ls_any_fills_from_sys_local_l2_pki_series = (
        ls_any_fills_from_sys_local_l2_series.div(inst_series)
    )
    return {
        "name": "Any L1 DC Fills from L2 PKI",
        "series": ls_any_fills_from_sys_local_l2_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_dc_fills_from_l3_or_different_l2_in_same_ccx_pki(grouped_df):
    ls_any_fills_from_sys_local_ccx_series = grouped_df.get_group(
        "ls_any_fills_from_sys.local_ccx"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_any_fills_from_sys_local_ccx_series.index = inst_series.index
    ls_any_fills_from_sys_local_ccx_pki_series = (
        ls_any_fills_from_sys_local_ccx_series.div(inst_series)
    )
    return {
        "name": "Any L1 DC Fills from L3 or different L2 in same CCX PKI",
        "series": ls_any_fills_from_sys_local_ccx_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_dc_fills_from_dram_pki(grouped_df):
    ls_any_fills_from_sys_dram_io_all_series = grouped_df.get_group(
        "ls_any_fills_from_sys.dram_io_all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_any_fills_from_sys_dram_io_all_series.index = inst_series.index
    ls_any_fills_from_sys_dram_io_all_pki_series = (
        ls_any_fills_from_sys_dram_io_all_series.div(inst_series)
    )
    return {
        "name": "Any L1 DC Fills from DRAM PKI",
        "series": ls_any_fills_from_sys_dram_io_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_any_l1_dc_fills_from_other_ccx_pki(grouped_df):
    ls_any_fills_from_sys_remote_cache_series = grouped_df.get_group(
        "ls_any_fills_from_sys.remote_cache"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_any_fills_from_sys_remote_cache_series.index = inst_series.index
    ls_any_fills_from_sys_remote_cache_pki_series = (
        ls_any_fills_from_sys_remote_cache_series.div(inst_series)
    )
    return {
        "name": "Any L1 DC Fills from Other CCX PKI",
        "series": ls_any_fills_from_sys_remote_cache_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_demand_l1_dc_fills_pki(grouped_df):
    ls_dmnd_fills_from_sys_all_series = grouped_df.get_group(
        "ls_dmnd_fills_from_sys.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_dmnd_fills_from_sys_all_series.index = inst_series.index
    ls_dmnd_fills_from_sys_all_pki_series = ls_dmnd_fills_from_sys_all_series.div(
        inst_series
    )
    return {
        "name": "Demand L1 DC Fills PKI",
        "series": ls_dmnd_fills_from_sys_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_hardware_prefetch_l1_dc_fills_pki(grouped_df):
    ls_hw_pf_dc_fills_all_series = grouped_df.get_group(
        "ls_hw_pf_dc_fills.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_hw_pf_dc_fills_all_series.index = inst_series.index
    ls_hw_pf_dc_fills_all_pki_series = ls_hw_pf_dc_fills_all_series.div(inst_series)
    return {
        "name": "Hardware Prefetch L1 DC Fills PKI",
        "series": ls_hw_pf_dc_fills_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_hardware_prefetch_l1_dc_fills_from_other_ccx_pki(grouped_df):
    ls_hw_pf_dc_fills_remote_cache_series = grouped_df.get_group(
        "ls_hw_pf_dc_fills.remote_cache"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_hw_pf_dc_fills_remote_cache_series.index = inst_series.index
    ls_hw_pf_dc_fills_remote_cache_pki_series = (
        ls_hw_pf_dc_fills_remote_cache_series.div(inst_series)
    )
    return {
        "name": "Hardware Prefetch L1 DC Fills from Other CCX PKI",
        "series": ls_hw_pf_dc_fills_remote_cache_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l3_miss_ptc(grouped_df):
    l3_lookup_state_l3_miss_series = grouped_df.get_group(
        "l3_lookup_state.l3_miss"
    ).counter_value
    aperf_series = grouped_df.get_group("cycles").counter_value
    l3_lookup_state_l3_miss_series.index = aperf_series.index
    l3_miss_ptc_series = l3_lookup_state_l3_miss_series.div(aperf_series) * 1000
    return {"name": "L3 Miss PTC", "series": l3_miss_ptc_series}


@skip_if_missing
def zen5_l3_miss_avg_load_to_use_latency_ns(grouped_df):
    l3_xi_sampled_latency_all_series = grouped_df.get_group(
        "l3_xi_sampled_latency.all"
    ).counter_value
    l3_xi_sampled_latency_requests_all_series = grouped_df.get_group(
        "l3_xi_sampled_latency_requests.all"
    ).counter_value
    l3_xi_sampled_latency_all_series.index = (
        l3_xi_sampled_latency_requests_all_series.index
    )
    latency_multiplier = 10
    l3_miss_avg_load_to_use_latency_ns_series = (
        l3_xi_sampled_latency_all_series.div(l3_xi_sampled_latency_requests_all_series)
        * latency_multiplier
    )
    return {
        "name": "L3 Miss Avg Load-to-Use Latency (ns)",
        "series": l3_miss_avg_load_to_use_latency_ns_series,
    }


@skip_if_missing
def zen5_l1_itlb_miss_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_hit_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_hit"
    ).counter_value
    bp_l1_tlb_miss_l2_tlb_miss_all_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.all"
    ).counter_value
    bp_l1_tlb_miss_l2_tlb_hit_series.index = bp_l1_tlb_miss_l2_tlb_miss_all_series.index
    l1_itlb_miss_series = (
        bp_l1_tlb_miss_l2_tlb_hit_series + bp_l1_tlb_miss_l2_tlb_miss_all_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    l1_itlb_miss_series.index = inst_series.index
    l1_itlb_miss_pki_series = l1_itlb_miss_series.div(inst_series)
    return {
        "name": "L1 iTLB Miss PKI",
        "series": l1_itlb_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_itlb_hit_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_hit_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_hit"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    bp_l1_tlb_miss_l2_tlb_hit_series.index = inst_series.index
    bp_l1_tlb_miss_l2_tlb_hit_pki_series = bp_l1_tlb_miss_l2_tlb_hit_series.div(
        inst_series
    )
    return {
        "name": "L2 iTLB Hit PKI",
        "series": bp_l1_tlb_miss_l2_tlb_hit_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_itlb_miss_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_miss_all_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    bp_l1_tlb_miss_l2_tlb_miss_all_series.index = inst_series.index
    bp_l1_tlb_miss_l2_tlb_miss_all_pki_series = (
        bp_l1_tlb_miss_l2_tlb_miss_all_series.div(inst_series)
    )
    return {
        "name": "L2 iTLB Miss PKI",
        "series": bp_l1_tlb_miss_l2_tlb_miss_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_4k_itlb_miss_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_miss_if4k_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.if4k"
    ).counter_value
    bp_l1_tlb_miss_l2_tlb_miss_coalesced_4k_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.coalesced_4k"
    ).counter_value
    bp_l1_tlb_miss_l2_tlb_miss_if4k_series.index = (
        bp_l1_tlb_miss_l2_tlb_miss_coalesced_4k_series.index
    )
    l2_4k_itlb_miss_series = (
        bp_l1_tlb_miss_l2_tlb_miss_if4k_series
        + bp_l1_tlb_miss_l2_tlb_miss_coalesced_4k_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_4k_itlb_miss_series.index = inst_series.index
    l2_4k_itlb_miss_pki_series = l2_4k_itlb_miss_series.div(inst_series)
    return {
        "name": "L2 4K iTLB Miss PKI",
        "series": l2_4k_itlb_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_2m_itlb_miss_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_miss_if2m_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.if2m"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    bp_l1_tlb_miss_l2_tlb_miss_if2m_series.index = inst_series.index
    bp_l1_tlb_miss_l2_tlb_miss_if2m_pki_series = (
        bp_l1_tlb_miss_l2_tlb_miss_if2m_series.div(inst_series)
    )
    return {
        "name": "L2 2M iTLB Miss PKI",
        "series": bp_l1_tlb_miss_l2_tlb_miss_if2m_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_1g_itlb_miss_pki(grouped_df):
    bp_l1_tlb_miss_l2_tlb_miss_if1g_series = grouped_df.get_group(
        "bp_l1_tlb_miss_l2_tlb_miss.if1g"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    bp_l1_tlb_miss_l2_tlb_miss_if1g_series.index = inst_series.index
    bp_l1_tlb_miss_l2_tlb_miss_if1g_pki_series = (
        bp_l1_tlb_miss_l2_tlb_miss_if1g_series.div(inst_series)
    )
    return {
        "name": "L2 1G iTLB Miss PKI",
        "series": bp_l1_tlb_miss_l2_tlb_miss_if1g_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_dtlb_miss_pki(grouped_df):
    dtlb_misses_series = grouped_df.get_group("dtlb_misses").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    dtlb_misses_series.index = inst_series.index
    dtlb_misses_pki_series = dtlb_misses_series.div(inst_series)
    return {"name": "dTLB Miss PKI", "series": dtlb_misses_pki_series, "prefix": 1000}


@skip_if_missing
def zen5_l1_dtlb_miss_pki(grouped_df):
    ls_l1_d_tlb_miss_all_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.all"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_all_series.index = inst_series.index
    ls_l1_d_tlb_miss_all_pki_series = ls_l1_d_tlb_miss_all_series.div(inst_series)
    return {
        "name": "L1 dTLB Miss PKI",
        "series": ls_l1_d_tlb_miss_all_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_dtlb_miss_pki(grouped_df):
    ls_l1_d_tlb_miss_all_l2_miss_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.all_l2_miss"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_all_l2_miss_series.index = inst_series.index
    ls_l1_d_tlb_miss_all_l2_miss_pki_series = ls_l1_d_tlb_miss_all_l2_miss_series.div(
        inst_series
    )
    return {
        "name": "L2 dTLB Miss PKI",
        "series": ls_l1_d_tlb_miss_all_l2_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_4k_dtlb_miss_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_4k_l2_miss_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss"
    ).counter_value
    ls_l1_d_tlb_miss_tlb_reload_coalesced_page_miss_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_coalesced_page_miss"
    ).counter_value
    ls_l1_d_tlb_miss_tlb_reload_4k_l2_miss_series.index = (
        ls_l1_d_tlb_miss_tlb_reload_coalesced_page_miss_series.index
    )
    l2_4k_dtlb_miss_series = (
        ls_l1_d_tlb_miss_tlb_reload_4k_l2_miss_series
        + ls_l1_d_tlb_miss_tlb_reload_coalesced_page_miss_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_4k_dtlb_miss_series.index = inst_series.index
    l2_4k_dtlb_miss_pki_series = l2_4k_dtlb_miss_series.div(inst_series)
    return {
        "name": "L2 4K dTLB Miss PKI",
        "series": l2_4k_dtlb_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_2m_dtlb_miss_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_miss_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_miss_series.index = inst_series.index
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_miss_pki_series = (
        ls_l1_d_tlb_miss_tlb_reload_2m_l2_miss_series.div(inst_series)
    )
    return {
        "name": "L2 2M dTLB Miss PKI",
        "series": ls_l1_d_tlb_miss_tlb_reload_2m_l2_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_1g_dtlb_miss_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_miss_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_1g_l2_miss"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_miss_series.index = inst_series.index
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_miss_pki_series = (
        ls_l1_d_tlb_miss_tlb_reload_1g_l2_miss_series.div(inst_series)
    )
    return {
        "name": "L2 1G dTLB Miss PKI",
        "series": ls_l1_d_tlb_miss_tlb_reload_1g_l2_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_4k_dtlb_hit_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_4k_l2_hit_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_4k_l2_hit"
    ).counter_value
    ls_l1_d_tlb_miss_tlb_reload_coalesced_page_hit_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_coalesced_page_hit"
    ).counter_value
    ls_l1_d_tlb_miss_tlb_reload_4k_l2_hit_series.index = (
        ls_l1_d_tlb_miss_tlb_reload_coalesced_page_hit_series.index
    )
    l2_4k_dtlb_hit_series = (
        ls_l1_d_tlb_miss_tlb_reload_4k_l2_hit_series
        + ls_l1_d_tlb_miss_tlb_reload_coalesced_page_hit_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_4k_dtlb_hit_series.index = inst_series.index
    l2_4k_dtlb_hit_pki_series = l2_4k_dtlb_hit_series.div(inst_series)
    return {
        "name": "L2 4K dTLB Hit PKI",
        "series": l2_4k_dtlb_hit_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_2m_dtlb_hit_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_hit_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_2m_l2_hit"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_hit_series.index = inst_series.index
    ls_l1_d_tlb_miss_tlb_reload_2m_l2_hit_pki_series = (
        ls_l1_d_tlb_miss_tlb_reload_2m_l2_hit_series.div(inst_series)
    )
    return {
        "name": "L2 2M dTLB Hit PKI",
        "series": ls_l1_d_tlb_miss_tlb_reload_2m_l2_hit_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_1g_dtlb_hit_pki(grouped_df):
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_hit_series = grouped_df.get_group(
        "ls_l1_d_tlb_miss.tlb_reload_1g_l2_hit"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_hit_series.index = inst_series.index
    ls_l1_d_tlb_miss_tlb_reload_1g_l2_hit_pki_series = (
        ls_l1_d_tlb_miss_tlb_reload_1g_l2_hit_series.div(inst_series)
    )
    return {
        "name": "L2 1G dTLB Hit PKI",
        "series": ls_l1_d_tlb_miss_tlb_reload_1g_l2_hit_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_all_l2_cache_misses_pki(grouped_df):
    l2_pf_miss_l2_hit_l3_all_series = grouped_df.get_group(
        "l2_pf_miss_l2_hit_l3.all"
    ).counter_value
    l2_pf_miss_l2_l3_all_series = grouped_df.get_group(
        "l2_pf_miss_l2_l3.all"
    ).counter_value
    l2_cache_req_stat_ic_dc_miss_in_l2_series = grouped_df.get_group(
        "l2_cache_req_stat.ic_dc_miss_in_l2"
    ).counter_value
    l2_pf_miss_l2_hit_l3_all_series.index = l2_pf_miss_l2_l3_all_series.index
    l2_cache_req_stat_ic_dc_miss_in_l2_series = l2_pf_miss_l2_l3_all_series.index
    all_l2_cache_misses_series = (
        l2_pf_miss_l2_hit_l3_all_series
        + l2_pf_miss_l2_l3_all_series
        + l2_cache_req_stat_ic_dc_miss_in_l2_series
    )
    inst_series = grouped_df.get_group("instructions").counter_value
    all_l2_cache_misses_series.index = inst_series.index
    all_l2_cache_misses_pki_series = all_l2_cache_misses_series.div(inst_series)
    return {
        "name": "All L2 Cache Misses PKI",
        "series": all_l2_cache_misses_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_l2_cache_miss_from_ic_fill_miss_pki(grouped_df):
    l2_cache_req_stat_ic_fill_miss_series = grouped_df.get_group(
        "l2_cache_req_stat.ic_fill_miss"
    ).counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    l2_cache_req_stat_ic_fill_miss_series.index = inst_series.index
    l2_cache_req_stat_ic_fill_miss_pki_series = (
        l2_cache_req_stat_ic_fill_miss_series.div(inst_series)
    )
    return {
        "name": "L2 Cache Miss from IC Fill Miss PKI",
        "series": l2_cache_req_stat_ic_fill_miss_pki_series,
        "prefix": 1000,
    }


@skip_if_missing
def zen5_total_dma_read_bw_mbs(grouped_df):
    iom0_upstream_read_beats_series = grouped_df.get_group(
        "iom0_upstream_read_beats"
    ).counter_value
    iom1_upstream_read_beats_series = grouped_df.get_group(
        "iom1_upstream_read_beats"
    ).counter_value
    iom2_upstream_read_beats_series = grouped_df.get_group(
        "iom2_upstream_read_beats"
    ).counter_value
    iom3_upstream_read_beats_series = grouped_df.get_group(
        "iom3_upstream_read_beats"
    ).counter_value
    iom4_upstream_read_beats_series = grouped_df.get_group(
        "iom4_upstream_read_beats"
    ).counter_value
    iom5_upstream_read_beats_series = grouped_df.get_group(
        "iom5_upstream_read_beats"
    ).counter_value
    iom6_upstream_read_beats_series = grouped_df.get_group(
        "iom6_upstream_read_beats"
    ).counter_value
    iom7_upstream_read_beats_series = grouped_df.get_group(
        "iom7_upstream_read_beats"
    ).counter_value
    iom0_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom1_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom2_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom3_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom4_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom5_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    iom6_upstream_read_beats_series.index = iom7_upstream_read_beats_series.index
    total_dma_read_bw_mbs_series = (
        (
            iom0_upstream_read_beats_series
            + iom1_upstream_read_beats_series
            + iom2_upstream_read_beats_series
            + iom3_upstream_read_beats_series
            + iom4_upstream_read_beats_series
            + iom5_upstream_read_beats_series
            + iom6_upstream_read_beats_series
            + iom7_upstream_read_beats_series
        )
        * 64
        * (10**-6)
    )
    duration_series = get_duration_series(
        grouped_df.get_group("iom7_upstream_read_beats")
    )
    return {
        "name": "Total DMA Read BW (MB/s)",
        "series": total_dma_read_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5_total_dma_write_bw_mbs(grouped_df):
    iom0_upstream_write_beats_series = grouped_df.get_group(
        "iom0_upstream_write_beats"
    ).counter_value
    iom1_upstream_write_beats_series = grouped_df.get_group(
        "iom1_upstream_write_beats"
    ).counter_value
    iom2_upstream_write_beats_series = grouped_df.get_group(
        "iom2_upstream_write_beats"
    ).counter_value
    iom3_upstream_write_beats_series = grouped_df.get_group(
        "iom3_upstream_write_beats"
    ).counter_value
    iom4_upstream_write_beats_series = grouped_df.get_group(
        "iom4_upstream_write_beats"
    ).counter_value
    iom5_upstream_write_beats_series = grouped_df.get_group(
        "iom5_upstream_write_beats"
    ).counter_value
    iom6_upstream_write_beats_series = grouped_df.get_group(
        "iom6_upstream_write_beats"
    ).counter_value
    iom7_upstream_write_beats_series = grouped_df.get_group(
        "iom7_upstream_write_beats"
    ).counter_value
    iom0_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom1_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom2_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom3_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom4_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom5_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    iom6_upstream_write_beats_series.index = iom7_upstream_write_beats_series.index
    total_dma_write_bw_mbs_series = (
        (
            iom0_upstream_write_beats_series
            + iom1_upstream_write_beats_series
            + iom2_upstream_write_beats_series
            + iom3_upstream_write_beats_series
            + iom4_upstream_write_beats_series
            + iom5_upstream_write_beats_series
            + iom6_upstream_write_beats_series
            + iom7_upstream_write_beats_series
        )
        * 64
        * (10**-6)
    )
    duration_series = get_duration_series(
        grouped_df.get_group("iom7_upstream_write_beats")
    )
    return {
        "name": "Total DMA Write BW (MB/s)",
        "series": total_dma_write_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5_dram_read_bw_mbs(grouped_df, num_cs_channels):
    cs0_dram_read_beats_series = grouped_df.get_group(
        "cs0_dram_read_beats"
    ).counter_value
    dram_read_bw_mbs_series = (
        cs0_dram_read_beats_series * num_cs_channels * 64 * (10**-6)
    )
    duration_series = get_duration_series(grouped_df.get_group("cs0_dram_read_beats"))
    return {
        "name": "DRAM Read BW (MB/s)",
        "series": dram_read_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5_dram_write_bw_mbs(grouped_df, num_cs_channels):
    cs0_dram_write_beats_series = grouped_df.get_group(
        "cs0_dram_write_beats"
    ).counter_value
    dram_write_bw_mbs_series = (
        cs0_dram_write_beats_series * num_cs_channels * 64 * (10**-6)
    )
    duration_series = get_duration_series(grouped_df.get_group("cs0_dram_write_beats"))
    return {
        "name": "DRAM Write BW (MB/s)",
        "series": dram_write_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5_dram_utilization_pct(grouped_df, ddr_freq, num_cs_channels):
    dram_read_bw_mbs_series = grouped_df.get_group("DRAM Read BW (MB/s)").counter_value
    dram_write_bw_mbs_series = grouped_df.get_group(
        "DRAM Write BW (MB/s)"
    ).counter_value
    dram_read_bw_mbs_series.index = dram_write_bw_mbs_series.index
    dram_utilization_pct_series = (
        (dram_read_bw_mbs_series + dram_write_bw_mbs_series)
        / (ddr_freq * 1000 * num_cs_channels * 0.008)
        * 100
    )
    return {"name": "DRAM Utilization %", "series": dram_utilization_pct_series}


@skip_if_missing
def zen5_total_cxl_read_bw_mbs(grouped_df):
    cs_cmp0_cxl_read_beats_series = grouped_df.get_group(
        "cs_cmp0_cxl_read_beats"
    ).counter_value
    cs_cmp1_cxl_read_beats_series = grouped_df.get_group(
        "cs_cmp1_cxl_read_beats"
    ).counter_value
    cs_cmp2_cxl_read_beats_series = grouped_df.get_group(
        "cs_cmp2_cxl_read_beats"
    ).counter_value
    cs_cmp3_cxl_read_beats_series = grouped_df.get_group(
        "cs_cmp3_cxl_read_beats"
    ).counter_value
    cs_cmp0_cxl_read_beats_series.index = cs_cmp3_cxl_read_beats_series.index
    cs_cmp1_cxl_read_beats_series.index = cs_cmp3_cxl_read_beats_series.index
    cs_cmp2_cxl_read_beats_series.index = cs_cmp3_cxl_read_beats_series.index
    total_cxl_read_bw_mbs_series = (
        (
            cs_cmp0_cxl_read_beats_series
            + cs_cmp1_cxl_read_beats_series
            + cs_cmp2_cxl_read_beats_series
            + cs_cmp3_cxl_read_beats_series
        )
        * 64
        * (10**-6)
    )
    duration_series = get_duration_series(
        grouped_df.get_group("cs_cmp3_cxl_read_beats")
    )
    return {
        "name": "Total CXL Read BW (MB/s)",
        "series": total_cxl_read_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5_total_cxl_write_bw_mbs(grouped_df):
    cs_cmp0_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp0_cxl_write_beats"
    ).counter_value
    cs_cmp1_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp1_cxl_write_beats"
    ).counter_value
    cs_cmp2_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp2_cxl_write_beats"
    ).counter_value
    cs_cmp3_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp3_cxl_write_beats"
    ).counter_value
    cs_cmp0_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index
    cs_cmp1_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index
    cs_cmp2_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index
    total_cxl_write_bw_mbs_series = (
        (
            cs_cmp0_cxl_write_beats_series
            + cs_cmp1_cxl_write_beats_series
            + cs_cmp2_cxl_write_beats_series
            + cs_cmp3_cxl_write_beats_series
        )
        * 64
        * (10**-6)
    )
    duration_series = get_duration_series(
        grouped_df.get_group("cs_cmp3_cxl_write_beats")
    )
    return {
        "name": "Total CXL Write BW (MB/s)",
        "series": total_cxl_write_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5es_mem_read_bw_MBps(grouped_df, ddr_freq):
    umc0_all_cyc = grouped_df.get_group("umc_data_cyc_umc0").counter_value
    umc0_cyc = grouped_df.get_group("umc_cyc_umc0").counter_value
    umc0_write_cyc = grouped_df.get_group("umc_data_write_cyc_umc0").counter_value
    umc0_cyc.index = umc0_all_cyc.index
    umc0_write_cyc.index = umc0_all_cyc.index
    umc0_read_cyc = umc0_all_cyc - umc0_write_cyc
    total_dram_read_bps = umc0_read_cyc.div(umc0_cyc)
    i = 1
    while (
        f"umc_data_cyc_umc{i}" in grouped_df.groups
        and f"umc_cyc_umc{i}" in grouped_df.groups
        and f"umc_data_write_cyc_umc{i}" in grouped_df.groups
    ):
        umci_all_cyc = grouped_df.get_group(f"umc_data_cyc_umc{i}").counter_value
        umci_cyc = grouped_df.get_group(f"umc_cyc_umc{i}").counter_value
        umci_write_cyc = grouped_df.get_group(
            f"umc_data_write_cyc_umc{i}"
        ).counter_value
        umci_all_cyc.index = umc0_all_cyc.index
        umci_cyc.index = umc0_all_cyc.index
        umci_write_cyc.index = umc0_all_cyc.index
        umci_read_cyc = umci_all_cyc - umci_write_cyc
        total_dram_read_bps += umci_read_cyc.div(umci_cyc)
        i += 1
    total_dram_read_mbps = 8 * ddr_freq / 2 * total_dram_read_bps
    return {
        "name": "DRAM Read BW (MB/s)",
        "series": total_dram_read_mbps,
    }


@skip_if_missing
def zen5es_mem_write_bw_MBps(grouped_df, ddr_freq):
    umc0_cyc = grouped_df.get_group("umc_cyc_umc0").counter_value
    umc0_write_cyc = grouped_df.get_group("umc_data_write_cyc_umc0").counter_value
    umc0_write_cyc.index = umc0_cyc.index
    total_dram_write_bps = umc0_write_cyc.div(umc0_cyc)
    i = 1
    while (
        f"umc_data_cyc_umc{i}" in grouped_df.groups
        and f"umc_cyc_umc{i}" in grouped_df.groups
        and f"umc_data_write_cyc_umc{i}" in grouped_df.groups
    ):
        umci_cyc = grouped_df.get_group(f"umc_cyc_umc{i}").counter_value
        umci_write_cyc = grouped_df.get_group(
            f"umc_data_write_cyc_umc{i}"
        ).counter_value
        umci_cyc.index = umc0_cyc.index
        umci_write_cyc.index = umc0_cyc.index
        total_dram_write_bps += umci_write_cyc.div(umci_cyc)
        i += 1
    total_dram_write_mbps = 8 * ddr_freq / 2 * total_dram_write_bps
    return {
        "name": "DRAM Write BW (MB/s)",
        "series": total_dram_write_mbps,
    }


@skip_if_missing
def zen5es_dram_utilization_pct(grouped_df, ddr_freq, num_cs_channels):
    dram_read_bw_mbs_series = grouped_df.get_group("DRAM Read BW (MB/s)").counter_value
    dram_write_bw_mbs_series = grouped_df.get_group(
        "DRAM Write BW (MB/s)"
    ).counter_value
    dram_read_bw_mbs_series.index = dram_write_bw_mbs_series.index
    dram_utilization_pct_series = (
        (dram_read_bw_mbs_series + dram_write_bw_mbs_series)
        / (ddr_freq * 1000 * num_cs_channels * 0.008)
        * 100
    )
    return {"name": "DRAM Utilization %", "series": dram_utilization_pct_series}


@skip_if_missing
def zen5es_total_cxl_read_bw_mbs(grouped_df):
    ccm0_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm0_0_cxl_read_beats"
    ).counter_value
    ccm0_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm0_1_cxl_read_beats"
    ).counter_value
    ccm1_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm1_0_cxl_read_beats"
    ).counter_value
    ccm1_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm1_1_cxl_read_beats"
    ).counter_value
    ccm2_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm2_0_cxl_read_beats"
    ).counter_value
    ccm2_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm2_1_cxl_read_beats"
    ).counter_value
    ccm3_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm3_0_cxl_read_beats"
    ).counter_value
    ccm3_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm3_1_cxl_read_beats"
    ).counter_value
    ccm4_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm4_0_cxl_read_beats"
    ).counter_value
    ccm4_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm4_1_cxl_read_beats"
    ).counter_value
    ccm5_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm5_0_cxl_read_beats"
    ).counter_value
    ccm5_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm5_1_cxl_read_beats"
    ).counter_value
    ccm6_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm6_0_cxl_read_beats"
    ).counter_value
    ccm6_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm6_1_cxl_read_beats"
    ).counter_value
    ccm7_0_cxl_read_beats_series = grouped_df.get_group(
        "ccm7_0_cxl_read_beats"
    ).counter_value
    ccm7_1_cxl_read_beats_series = grouped_df.get_group(
        "ccm7_1_cxl_read_beats"
    ).counter_value

    ccm0_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm0_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm1_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm1_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm2_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm2_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm3_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm3_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm4_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm4_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm5_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm5_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm6_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm6_1_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    ccm7_0_cxl_read_beats_series.index = ccm7_1_cxl_read_beats_series.index
    total_cxl_read_bw_mbs_series = (
        (
            ccm0_0_cxl_read_beats_series
            + ccm0_1_cxl_read_beats_series
            + ccm1_0_cxl_read_beats_series
            + ccm1_1_cxl_read_beats_series
            + ccm2_0_cxl_read_beats_series
            + ccm2_1_cxl_read_beats_series
            + ccm3_0_cxl_read_beats_series
            + ccm3_1_cxl_read_beats_series
            + ccm4_0_cxl_read_beats_series
            + ccm4_1_cxl_read_beats_series
            + ccm5_0_cxl_read_beats_series
            + ccm5_1_cxl_read_beats_series
            + ccm6_0_cxl_read_beats_series
            + ccm6_1_cxl_read_beats_series
            + ccm7_0_cxl_read_beats_series
            + ccm7_1_cxl_read_beats_series
        )
        * 32
        * (10**-6)
    )
    duration_series = get_duration_series(grouped_df.get_group("ccm7_1_cxl_read_beats"))
    return {
        "name": "Total CXL Read BW (MB/s)",
        "series": total_cxl_read_bw_mbs_series.div(duration_series),
    }


@skip_if_missing
def zen5es_total_cxl_write_bw_mbs(grouped_df):
    cs_cmp0_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp0_cxl_write_beats"
    ).counter_value
    cs_cmp1_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp1_cxl_write_beats"
    ).counter_value
    cs_cmp2_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp2_cxl_write_beats"
    ).counter_value
    cs_cmp3_cxl_write_beats_series = grouped_df.get_group(
        "cs_cmp3_cxl_write_beats"
    ).counter_value
    cs_cmp0_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index
    cs_cmp1_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index
    cs_cmp2_cxl_write_beats_series.index = cs_cmp3_cxl_write_beats_series.index

    total_cxl_write_bw_mbs_series = (
        (
            cs_cmp0_cxl_write_beats_series
            + cs_cmp1_cxl_write_beats_series
            + cs_cmp2_cxl_write_beats_series
            + cs_cmp3_cxl_write_beats_series
        )
        * 64
        * (10**-6)
    )
    duration_series = get_duration_series(
        grouped_df.get_group("cs_cmp3_cxl_write_beats")
    )

    return {
        "name": "Total CXL Write BW (MB/s)",
        "series": total_cxl_write_bw_mbs_series.div(duration_series),
    }


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


def get_memory_info():
    """
    Returns the number of memory channels and the DDR frequency.
    Returns:
        tuple: A tuple containing the number of memory channels (int) and the DDR frequency (str).
    """
    # Run dmidecode command to get memory device information
    dmidecode_out = subprocess.check_output(
        ["sudo", "dmidecode", "--type", "17"]
    ).decode("utf-8")
    # Parse the output to find the relevant lines
    lines = dmidecode_out.split("\n")
    relevant_lines = []
    p = False
    for line in lines:
        if line.strip() == "Memory Device":
            p = True
        elif line.strip() == "":
            p = False
        elif p:
            relevant_lines.append(line)
    # Filter out lines containing "cxl" and count the remaining lines with "Bank Locator"
    num_channels = 0
    for line in relevant_lines:
        if "Bank Locator" in line and any(
            x in line.lower() for x in ["p0", "socket 0", "node0"]
        ):
            num_channels += 1
    # Get the DDR frequency
    ddr_freq_out = subprocess.check_output(["sudo", "dmidecode"]).decode("utf-8")
    ddr_freq_lines = ddr_freq_out.split("\n")
    for line in ddr_freq_lines:
        if "Configured Memory Speed" in line:
            ddr_freq = int(line.split(":")[1].strip().split()[0])
            break
    return num_channels, ddr_freq


@click.command()
@click.argument(
    "amd_perf_csv_file", type=click.Path(exists=True, dir_okay=False, resolve_path=True)
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
@click.option(
    "-a",
    "--arch",
    type=click.Choice(["zen3", "zen4", "zen5", "zen5es"]),
    default="zen3",
    help="Specify which AMD architecture to aggregate counters correctly.",
)
def main(
    amd_perf_csv_file: click.Path,
    series: typing.TextIO,
    format: click.Choice,
    arch: click.Choice,
) -> None:
    df = read_csv(amd_perf_csv_file)
    grouped_df = df.groupby("event_name")
    if arch == "zen5" or arch == "zen5es":
        num_channels, ddr_freq = get_memory_info()
        metrics = [
            # core metrics
            timestamp(grouped_df),
            mips(grouped_df),
            ipc(grouped_df),
            zen5_user_instr_pct(grouped_df),
            zen5_kernel_instr_pct(grouped_df),
            zen5_overall_utilization_pct(grouped_df),
            zen5_ops_per_instruction(grouped_df),
            zen5_dispatched_ops_per_cycle(grouped_df),
            zen5_dispatched_ops_per_cycle_v2(grouped_df),
            zen5_microcoded_pki(grouped_df),
            zen5_microcoded_uops_pct(grouped_df),
            zen5_interrupts_pki(grouped_df),
            zen5_opcache_ops_pki(grouped_df),
            zen5_decoder_ops_pki(grouped_df),
            zen5_decoder_ops_pki_v2(grouped_df),
            zen5_frontend_bound_pct(grouped_df),
            zen5_frontend_bound_by_latency_pct(grouped_df),
            zen5_frontend_bound_by_bandwidth_pct(
                zen5_frontend_bound_pct(grouped_df),
                zen5_frontend_bound_by_latency_pct(grouped_df),
            ),
            zen5_backend_bound_pct(grouped_df),
            zen5_backend_bound_by_memory_pct(grouped_df),
            zen5_backend_bound_by_cpu_pct(
                zen5_backend_bound_pct(grouped_df),
                zen5_backend_bound_by_memory_pct(grouped_df),
            ),
            zen5_bad_speculation_pct(grouped_df),
            zen5_retiring_pct(grouped_df),
            zen5_smt_contention_pct(grouped_df),
            zen5_op_queue_empty_pki(grouped_df),
            zen5_token_stall_pki(grouped_df),
            zen5_branch_retired_pki(grouped_df),
            zen5_branch_retired_mispred_pki(grouped_df),
            zen5_branch_retired_taken_pki(grouped_df),
            zen5_branch_retired_indirect_mispred_pki(grouped_df),
            zen5_branch_retired_conditional_pki(grouped_df),
            zen5_branch_retired_conditional_mispred_pki(grouped_df),
            zen5_branch_retired_direct_jump_call_pki(grouped_df),
            zen5_branch_retired_indirect_jump_pki(grouped_df),
            zen5_branch_retired_near_return_pki(grouped_df),
            zen5_branch_retired_near_return_mispred_pki(grouped_df),
            zen5_fp_instr_retired_pki(grouped_df),
            zen5_fp_sse_avx_instr_retired_pki(grouped_df),
            zen5_ls_uop_disp_ld_pki(grouped_df),
            zen5_ls_uop_disp_st_pki(grouped_df),
            zen5_os_locks_pki(grouped_df),
            zen5_user_locks_pki(grouped_df),
            zen5_l1_icache_miss_pct(grouped_df),
            zen5_any_l1_ic_fills_pki(grouped_df),
            zen5_any_l1_ic_fills_from_l2_pki(grouped_df),
            zen5_any_l1_ic_fills_from_l3_or_different_l2_in_same_ccx_pki(grouped_df),
            zen5_any_l1_ic_fills_from_dram_pki(grouped_df),
            zen5_any_l1_ic_fills_from_other_ccx_pki(grouped_df),
            zen5_any_l1_ic_fills_from_l2_miss_pct(grouped_df),
            zen5_any_l1_ic_fills_from_l2_miss_pki(grouped_df),
            zen5_any_l1_dc_fills_pki(grouped_df),
            zen5_any_l1_dc_fills_from_l2_pki(grouped_df),
            zen5_any_l1_dc_fills_from_l3_or_different_l2_in_same_ccx_pki(grouped_df),
            zen5_any_l1_dc_fills_from_dram_pki(grouped_df),
            zen5_any_l1_dc_fills_from_other_ccx_pki(grouped_df),
            zen5_demand_l1_dc_fills_pki(grouped_df),
            zen5_hardware_prefetch_l1_dc_fills_pki(grouped_df),
            zen5_hardware_prefetch_l1_dc_fills_from_other_ccx_pki(grouped_df),
            zen5_l3_miss_ptc(grouped_df),
            zen5_l3_miss_avg_load_to_use_latency_ns(grouped_df),
            zen5_l1_itlb_miss_pki(grouped_df),
            zen5_l2_itlb_hit_pki(grouped_df),
            zen5_l2_itlb_miss_pki(grouped_df),
            zen5_l2_4k_itlb_miss_pki(grouped_df),
            zen5_l2_2m_itlb_miss_pki(grouped_df),
            zen5_l2_1g_itlb_miss_pki(grouped_df),
            zen5_dtlb_miss_pki(grouped_df),
            zen5_l1_dtlb_miss_pki(grouped_df),
            zen5_l2_dtlb_miss_pki(grouped_df),
            zen5_l2_4k_dtlb_miss_pki(grouped_df),
            zen5_l2_2m_dtlb_miss_pki(grouped_df),
            zen5_l2_1g_dtlb_miss_pki(grouped_df),
            zen5_l2_4k_dtlb_hit_pki(grouped_df),
            zen5_l2_2m_dtlb_hit_pki(grouped_df),
            zen5_l2_1g_dtlb_hit_pki(grouped_df),
            zen5_all_l2_cache_misses_pki(grouped_df),
            zen5_l2_cache_miss_from_ic_fill_miss_pki(grouped_df),
            zen5_total_dma_read_bw_mbs(grouped_df),
            zen5_total_dma_write_bw_mbs(grouped_df),
        ]
        if arch == "zen5":
            # zen5 metrics
            metrics += [
                zen5_dram_read_bw_mbs(grouped_df, num_channels),
                zen5_dram_write_bw_mbs(grouped_df, num_channels),
                zen5_dram_utilization_pct(grouped_df, ddr_freq, num_channels),
                zen5_total_cxl_read_bw_mbs(grouped_df),
                zen5_total_cxl_write_bw_mbs(grouped_df),
            ]
        elif arch == "zen5es":
            # zen5es metrics
            metrics += [
                zen5es_mem_read_bw_MBps(grouped_df, ddr_freq),
                zen5es_mem_write_bw_MBps(grouped_df, ddr_freq),
                zen5es_dram_utilization_pct(grouped_df, ddr_freq, num_channels),
                zen5es_total_cxl_read_bw_mbs(grouped_df),
                zen5es_total_cxl_write_bw_mbs(grouped_df),
            ]
    else:
        metrics = [
            timestamp(grouped_df),
            mips(grouped_df),
            ipc(grouped_df),
            uops_per_instructions(grouped_df),
            uops_dispatched_opcache_per_instructions(grouped_df),
            uops_dispatched_decoder_per_instructions(grouped_df),
            frontend_stalls(grouped_df),
            frontend_stalls_due_to_ic_miss(grouped_df),
            backend_stalls(grouped_df),
            branch_mispred_rate(grouped_df),
            avg_mab_latency(grouped_df),
            l1_icache_mab_demand_requests_rate(grouped_df),
            l1_icache_mab_prefetch_requests_rate(grouped_df),
            l1_icache_miss_rate(grouped_df),
            l1_icache_mpki(grouped_df),
            l1_icache_fills_l2_ratio(grouped_df),
            l1_icache_fills_sys_ratio(grouped_df),
            l1_dcache_miss_rate(grouped_df),
            l1_dcache_mpki(grouped_df),
            l2_code_miss_rate(grouped_df),
            l2_code_mpki(grouped_df),
            l2_data_miss_rate(grouped_df),
            l2_data_mpki(grouped_df),
            llc_miss_rate(grouped_df),
            llc_mpki(grouped_df),
            llc_avg_load_to_use_lat_clks(grouped_df),
            itlb_mpki(grouped_df),
            l2_itlb_mpki(grouped_df),
            l2_4k_itlb_mpki(grouped_df),
            l2_2m_itlb_mpki(grouped_df),
            l2_1g_itlb_mpki(grouped_df),
            dtlb_mpki(grouped_df),
            l1_4k_dtlb_mpki(grouped_df),
            l1_2m_dtlb_mpki(grouped_df),
            l1_1g_dtlb_mpki(grouped_df),
            l2_dtlb_mpki(grouped_df),
        ]

    if arch == "zen3":
        metrics.append(mem_read_bw_MBps(grouped_df))
        metrics.append(mem_write_bw_MBps(grouped_df))
    elif arch == "zen4":
        metrics.append(zen4_mem_read_bw_MBps(grouped_df))
        metrics.append(zen4_mem_write_bw_MBps(grouped_df))

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
