#!/usr/bin/env python3

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
    type=click.Choice(["zen3", "zen4"]),
    default="zen3",
    help="Specify which AMD architecture to aggregate counters correctly.",
)
def main(
    amd_perf_csv_file: click.Path,
    series: click.File,
    format: click.Choice,
    arch: click.Choice,
) -> None:
    df = read_csv(amd_perf_csv_file)
    grouped_df = df.groupby("event_name")
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
