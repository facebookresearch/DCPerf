#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""AMD Zen4 (EPYC Genoa/Bergamo) -- core PMU + memory bandwidth + top-down"""

try:
    from cea.chips.benchpress.perfutils.amd_perf.core import register_arch
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from amd_perf.core import register_arch  # pyre-ignore[21]
try:
    from cea.chips.benchpress.perfutils.amd_perf.metrics import (
        avg_mab_latency,
        backend_stalls,
        branch_mispred_rate,
        dtlb_mpki,
        frontend_stalls,
        frontend_stalls_due_to_ic_miss,
        ipc,
        itlb_mpki,
        l1_1g_dtlb_mpki,
        l1_2m_dtlb_mpki,
        l1_4k_dtlb_mpki,
        l1_dcache_miss_rate,
        l1_dcache_mpki,
        l1_icache_fills_l2_ratio,
        l1_icache_fills_sys_ratio,
        l1_icache_mab_demand_requests_rate,
        l1_icache_mab_prefetch_requests_rate,
        l1_icache_miss_rate,
        l1_icache_mpki,
        l2_1g_itlb_mpki,
        l2_2m_itlb_mpki,
        l2_4k_itlb_mpki,
        l2_code_miss_rate,
        l2_code_mpki,
        l2_data_miss_rate,
        l2_data_mpki,
        l2_dtlb_mpki,
        l2_itlb_mpki,
        llc_avg_load_to_use_lat_clks,
        llc_miss_rate,
        llc_mpki,
        mips,
        timestamp,
        uops_dispatched_decoder_per_instructions,
        uops_dispatched_opcache_per_instructions,
        uops_per_instructions,
        zen4_backend_bound,
        zen4_frontend_bound,
        zen4_mem_read_bw_MBps,
        zen4_mem_write_bw_MBps,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from amd_perf.metrics import (  # pyre-ignore[21]
        avg_mab_latency,
        backend_stalls,
        branch_mispred_rate,
        dtlb_mpki,
        frontend_stalls,
        frontend_stalls_due_to_ic_miss,
        ipc,
        itlb_mpki,
        l1_1g_dtlb_mpki,
        l1_2m_dtlb_mpki,
        l1_4k_dtlb_mpki,
        l1_dcache_miss_rate,
        l1_dcache_mpki,
        l1_icache_fills_l2_ratio,
        l1_icache_fills_sys_ratio,
        l1_icache_mab_demand_requests_rate,
        l1_icache_mab_prefetch_requests_rate,
        l1_icache_miss_rate,
        l1_icache_mpki,
        l2_1g_itlb_mpki,
        l2_2m_itlb_mpki,
        l2_4k_itlb_mpki,
        l2_code_miss_rate,
        l2_code_mpki,
        l2_data_miss_rate,
        l2_data_mpki,
        l2_dtlb_mpki,
        l2_itlb_mpki,
        llc_avg_load_to_use_lat_clks,
        llc_miss_rate,
        llc_mpki,
        mips,
        timestamp,
        uops_dispatched_decoder_per_instructions,
        uops_dispatched_opcache_per_instructions,
        uops_per_instructions,
        zen4_backend_bound,
        zen4_frontend_bound,
        zen4_mem_read_bw_MBps,
        zen4_mem_write_bw_MBps,
    )


def metrics(grouped_df):
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
    metrics.append(zen4_mem_read_bw_MBps(grouped_df))
    metrics.append(zen4_mem_write_bw_MBps(grouped_df))
    metrics.append(zen4_frontend_bound(grouped_df))
    metrics.append(zen4_backend_bound(grouped_df))
    return metrics


register_arch(
    "zen4",
    metrics,
    vendor="amd",
    align="longest",
    description="Zen4 (EPYC Genoa/Bergamo) -- core PMU + memory bandwidth + top-down",
)
