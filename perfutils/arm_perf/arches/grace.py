#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""NVIDIA Grace (Neoverse V2) metrics.

Core PMU + SCF uncore. Collect with collect_nvda_neoversev2_perf_counters.sh.
Cycles counter is named ``cpu_cycles`` (differs from Neoverse V3 / Axion).
"""

try:
    from cea.chips.benchpress.perfutils.arm_perf.core import (
        get_duration_series,
        register_arch,
        skip_if_missing,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf.core import (  # pyre-ignore[21]
        get_duration_series,
        register_arch,
        skip_if_missing,
    )


@skip_if_missing
def grace_timestamp(grouped_df):
    ts_series = grouped_df.get_group("cpu_cycles").timestamp
    return {"name": "Timestamp_Secs", "series": ts_series}


@skip_if_missing
def grace_duration(grouped_df):
    duration_series = get_duration_series(grouped_df.get_group("duration_time"))

    return {
        "name": "Per-Sample Effective Sampling Duration (msecs)",
        "series": duration_series,
        "prefix": 10**-6,
    }


@skip_if_missing
def grace_mips(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = get_duration_series(grouped_df.get_group("instructions"))
    return {
        "name": "MIPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def grace_muopps(grouped_df):
    inst_series = grouped_df.get_group("r3A").counter_value
    duration_series = get_duration_series(grouped_df.get_group("r3A"))
    return {
        "name": "MuOPPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def grace_ipc(grouped_df):
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    cycles_series.index = inst_series.index

    ipc_series = inst_series.div(cycles_series)
    return {"name": "IPC", "series": ipc_series}


@skip_if_missing
def grace_int_inst_percent(grouped_df):
    int_inst_series = grouped_df.get_group("r73").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    int_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    int_inst_percent_series = int_inst_series / inst_series
    return {
        "name": "INT instruction %",
        "series": int_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_simd_inst_percent(grouped_df):
    simd_inst_series = grouped_df.get_group("r74").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    simd_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    simd_inst_percent_series = simd_inst_series / inst_series
    return {
        "name": "SIMD instruction %",
        "series": simd_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_fp_inst_percent(grouped_df):
    fp_inst_series = grouped_df.get_group("r75").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    fp_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    fp_inst_percent_series = fp_inst_series / inst_series
    return {
        "name": "FP instruction %",
        "series": fp_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_ld_inst_percent(grouped_df):
    ld_inst_series = grouped_df.get_group("r70").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    ld_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    ld_inst_percent_series = ld_inst_series / inst_series
    return {
        "name": "Load instruction %",
        "series": ld_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_st_inst_percent(grouped_df):
    st_inst_series = grouped_df.get_group("r71").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    st_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    st_inst_percent_series = st_inst_series / inst_series
    return {
        "name": "Store instruction %",
        "series": st_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_crypto_inst_percent(grouped_df):
    crypto_inst_series = grouped_df.get_group("r77").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    crypto_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    crypto_inst_percent_series = crypto_inst_series / inst_series
    return {
        "name": "Crypto instruction %",
        "series": crypto_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_branch_inst_percent(grouped_df):
    branch_imm_inst_series = grouped_df.get_group("r78").counter_value
    branch_ind_inst_series = grouped_df.get_group("r7a").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = grouped_df.get_group("duration_time").counter_value

    branch_imm_inst_series.index = duration_series.index
    branch_ind_inst_series.index = duration_series.index
    inst_series.index = duration_series.index

    branch_inst_percent_series = (
        branch_imm_inst_series + branch_ind_inst_series
    ) / inst_series
    return {
        "name": "Branch instruction %",
        "series": branch_inst_percent_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_gflops(grouped_df):
    fp_scale_series = grouped_df.get_group("r80C0").counter_value  # FP_SCALE_OPS_SPEC
    fp_fixed_series = grouped_df.get_group("r80C1").counter_value  # FP_FIXED_OPS_SPEC
    duration_series = get_duration_series(grouped_df.get_group("r80C0"))

    fp_scale_series.index = duration_series.index
    fp_fixed_series.index = duration_series.index

    flop_sum_series = fp_fixed_series + fp_scale_series
    gflops_series = flop_sum_series.div(duration_series) / 10**9
    return {
        "name": "GFLOPS (any precision incl SVE)",
        "series": gflops_series,
    }


@skip_if_missing
def grace_sve_gflops(grouped_df):
    fp_scale_series = grouped_df.get_group("r80C1").counter_value  # FP_FIXED_OPS_SPEC
    duration_series = get_duration_series(grouped_df.get_group("r80C1"))

    fp_scale_series.index = duration_series.index

    sve_gflops_series = fp_scale_series.div(duration_series) / 10**9
    return {
        "name": "SVE GFLOPS (any precision)",
        "series": sve_gflops_series,
    }


@skip_if_missing
def grace_branch_mpki(grouped_df):
    branch_refill_series = grouped_df.get_group(
        "r22"
    ).counter_value  # BR_MIS_PRED_RETIRED
    instructions_series = grouped_df.get_group("instructions").counter_value

    branch_refill_series.index = instructions_series.index
    return {
        "name": "Branch MPKI",
        "series": branch_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_branch_miss_rate(grouped_df):
    branch_miss_series = grouped_df.get_group(
        "r22"  # BR_MIS_PRED_RETIRED
    ).counter_value
    branch_series = grouped_df.get_group("r21").counter_value  # BR_RETIRED
    instructions_series = grouped_df.get_group("instructions").counter_value

    branch_miss_series.index = instructions_series.index
    branch_series.index = instructions_series.index
    branch_miss_rate_series = branch_miss_series / branch_series

    return {
        "name": "Branch Miss Rate %",
        "series": branch_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_l1_icache_mpki(grouped_df):
    icache_refill_series = grouped_df.get_group("r01").counter_value  # L1I_CACHE_LMISS
    instructions_series = grouped_df.get_group("instructions").counter_value

    icache_refill_series.index = instructions_series.index
    return {
        "name": "L1 iCache MPKI",
        "series": icache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l1_icache_miss_rate(grouped_df):
    l1_icache_miss_rd_series = grouped_df.get_group(
        "r01"  # L1i_CACHE_LMISS
    ).counter_value
    l1_icache_rd_series = grouped_df.get_group("r14").counter_value  # L1i_CACHE
    duration_series = grouped_df.get_group("duration_time").counter_value

    l1_icache_miss_rd_series.index = duration_series.index
    l1_icache_rd_series.index = duration_series.index
    l1_icache_miss_rate_series = l1_icache_miss_rd_series / l1_icache_rd_series

    return {
        "name": "L1 iCache Miss Rate %",
        "series": l1_icache_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_l1_dcache_mpki(grouped_df):
    dcache_refill_series = grouped_df.get_group("r03").counter_value  # L1D_CACHE_LMISS
    instructions_series = grouped_df.get_group("instructions").counter_value

    dcache_refill_series.index = instructions_series.index
    return {
        "name": "L1 dCache MPKI",
        "series": dcache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l1_dcache_miss_rate(grouped_df):
    l1_dcache_miss_rd_series = grouped_df.get_group(
        "r03"  # L1D_CACHE_LMISS
    ).counter_value
    l1_dcache_rd_series = grouped_df.get_group("r04").counter_value  # L1D_CACHE
    duration_series = grouped_df.get_group("duration_time").counter_value

    l1_dcache_miss_rd_series.index = duration_series.index
    l1_dcache_rd_series.index = duration_series.index
    l1_dcache_miss_rate_series = l1_dcache_miss_rd_series / l1_dcache_rd_series

    return {
        "name": "L1 dCache Miss Rate %",
        "series": l1_dcache_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_l2_cache_mpki(grouped_df):
    l2_dcache_refill_series = grouped_df.get_group(
        "r17"  # L2D_CACHE_REFILL
    ).counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l2_dcache_refill_series.index = instructions_series.index
    return {
        "name": "L2 Cache MPKI",
        "series": l2_dcache_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l2_cache_code_mpki(grouped_df):
    l2_cache_code_refill_series = grouped_df.get_group("r108").counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l2_cache_code_refill_series.index = instructions_series.index
    return {
        "name": "L2 Cache Code MPKI",
        "series": l2_cache_code_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l2_cache_miss_rate(grouped_df):
    l2_cache_miss_rd_series = grouped_df.get_group(
        "r17"  # L2D_CACHE_REFILL
    ).counter_value
    l2_cache_rd_series = grouped_df.get_group("r16").counter_value  # L2D_CACHE
    duration_series = grouped_df.get_group("duration_time").counter_value

    l2_cache_miss_rd_series.index = duration_series.index
    l2_cache_rd_series.index = duration_series.index
    l2_cache_miss_rate_series = l2_cache_miss_rd_series / l2_cache_rd_series

    return {
        "name": "L2 Cache Miss Rate %",
        "series": l2_cache_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_l3_cache_mpki(grouped_df):
    l3_cache_miss_rd_series = grouped_df.get_group(
        "r37"  # LL_CACHE_MISS_RD
    ).counter_value
    instructions_series = grouped_df.get_group("instructions").counter_value

    l3_cache_miss_rd_series.index = instructions_series.index
    return {
        "name": "L3 Cache MPKI",
        "series": l3_cache_miss_rd_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l3_cache_miss_rate(grouped_df):
    l3_cache_miss_rd_series = grouped_df.get_group(
        "r37"  # LL_CACHE_MISS_RD
    ).counter_value
    l3_cache_rd_series = grouped_df.get_group("r36").counter_value  # LL_CACHE_MISS_RD
    duration_series = grouped_df.get_group("duration_time").counter_value

    l3_cache_miss_rd_series.index = duration_series.index
    l3_cache_rd_series.index = duration_series.index
    l3_cache_miss_rate_series = l3_cache_miss_rd_series / l3_cache_rd_series

    return {
        "name": "L3 Cache Miss Rate %",
        "series": l3_cache_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_itlb_mpki(grouped_df):
    l1i_tlb_refill_series = grouped_df.get_group("r02").counter_value  # L1I_TLB_REFILL
    instructions_series = grouped_df.get_group("instructions").counter_value

    l1i_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L1 iTLB MPKI",
        "series": l1i_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_itlb_miss_rate(grouped_df):
    itlb_miss_rd_series = grouped_df.get_group("r02").counter_value  # L1I_TLB_REFILL
    itlb_rd_series = grouped_df.get_group("r26").counter_value  # L1I_TLB
    duration_series = grouped_df.get_group("duration_time").counter_value

    itlb_miss_rd_series.index = duration_series.index
    itlb_rd_series.index = duration_series.index
    itlb_miss_rate_series = itlb_miss_rd_series / itlb_rd_series

    return {
        "name": "L1 iTLB Miss Rate %",
        "series": itlb_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_dtlb_mpki(grouped_df):
    l1d_tlb_refill_series = grouped_df.get_group("r05").counter_value  # L1D_TLB_REFILL
    instructions_series = grouped_df.get_group("instructions").counter_value

    l1d_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L1 dTLB MPKI",
        "series": l1d_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_dtlb_miss_rate(grouped_df):
    dtlb_miss_rd_series = grouped_df.get_group("r05").counter_value  # L1D_TLB_REFILL
    dtlb_rd_series = grouped_df.get_group("r25").counter_value  # L1D_TLB
    duration_series = grouped_df.get_group("duration_time").counter_value

    dtlb_miss_rd_series.index = duration_series.index
    dtlb_rd_series.index = duration_series.index
    dtlb_miss_rate_series = dtlb_miss_rd_series / dtlb_rd_series

    return {
        "name": "L1 dTLB Miss Rate %",
        "series": dtlb_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_l2tlb_mpki(grouped_df):
    l2_tlb_refill_series = grouped_df.get_group("r2D").counter_value  # L2D_TLB_REFILL
    instructions_series = grouped_df.get_group("instructions").counter_value

    l2_tlb_refill_series.index = instructions_series.index

    return {
        "name": "L2 TLB MPKI",
        "series": l2_tlb_refill_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_l2tlb_miss_rate(grouped_df):
    l2tlb_miss_rd_series = grouped_df.get_group("r2D").counter_value  # L2D_TLB_REFILL
    l2tlb_rd_series = grouped_df.get_group("r2F").counter_value  # L2D_TLB
    duration_series = grouped_df.get_group("duration_time").counter_value

    l2tlb_miss_rd_series.index = duration_series.index
    l2tlb_rd_series.index = duration_series.index

    l2tlb_miss_rate_series = l2tlb_miss_rd_series / l2tlb_rd_series

    return {
        "name": "L2 TLB Miss Rate %",
        "series": l2tlb_miss_rate_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_itlb_walk_mpki(grouped_df):
    itlb_walk_series = grouped_df.get_group("r35").counter_value  # ITLB_WALK
    instructions_series = grouped_df.get_group("instructions").counter_value

    itlb_walk_series.index = instructions_series.index

    return {
        "name": "iTLB Walk MPKI",
        "series": itlb_walk_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_dtlb_walk_mpki(grouped_df):
    dtlb_walk_series = grouped_df.get_group("r34").counter_value  # DTLB_WALK
    instructions_series = grouped_df.get_group("instructions").counter_value

    dtlb_walk_series.index = instructions_series.index

    return {
        "name": "dTLB Walk MPKI",
        "series": dtlb_walk_series.div(instructions_series / 1000.0),
    }


@skip_if_missing
def grace_retiring_slots(grouped_df):
    op_retired_series = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec_series = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot_series = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    op_retired_series.index = cycles_series.index
    op_spec_series.index = cycles_series.index
    stall_slot_series.index = cycles_series.index

    retiring_slots_series = (op_retired_series / op_spec_series) * (
        1 - (stall_slot_series / (8 * cycles_series))
    )
    return {
        "name": "TopDown Retiring %",
        "series": retiring_slots_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_frontend_bound_slots(grouped_df):
    stall_slot_fe_series = grouped_df.get_group(
        "r3E"  # STALL_SLOT_FRONTEND
    ).counter_value
    br_mis_pred_series = grouped_df.get_group("r10").counter_value  # BR_MISPRED
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    stall_slot_fe_series.index = cycles_series.index
    br_mis_pred_series.index = cycles_series.index

    fe_bound_series = (stall_slot_fe_series / (8 * cycles_series)) - (
        br_mis_pred_series / cycles_series
    )
    return {
        "name": "TopDown FrontendBound %",
        "series": fe_bound_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_backend_bound_slots(grouped_df):
    stall_slot_be_series = grouped_df.get_group(
        "r3D"  # STALL_SLOT_BACKEND
    ).counter_value
    br_mis_pred_series = grouped_df.get_group("r10").counter_value  # BR_MISPRED
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    stall_slot_be_series.index = cycles_series.index
    br_mis_pred_series.index = cycles_series.index

    be_bound_series = (
        stall_slot_be_series / (8 * cycles_series)
        - br_mis_pred_series * 3 / cycles_series
    )
    return {
        "name": "TopDown BackendBound %",
        "series": be_bound_series,
        "prefix": 100,
    }


"""
Important Note: The stall_slot_backend and stall_slot_frontend events are not reliable for accurately
measuring backend and frontend boundness on NVIDIA Grace. For more information, refer to the ARM documentation:
https://developer.arm.com/documentation/SDEN2332927/latest/
These counters are also used in TopdownL1 metrics in perf, which are similarly inaccurate.
We recommend using stall_backend and stall_frontend for better accuracy.
Cycle-based metrics differ from stall-based metrics. Stall-based metrics increase with each slot
that stalls in the frontend/backend, while cycle-based metrics increase when no instruction retires
in a cycle. Thus, cycle-based metrics are generally lower, with retiring + backend + frontend + bad_speculation < 100%.
Cycle-based metrics typically have lower values, with the sum of retiring, backend, frontend, and bad speculation being less than 100%.
However, when adding "frontend_backend_boundness" (a combination of stall_slot_backend and stall_slot_frontend) to retiring and bad speculation, the total will equal 100%.
"""


@skip_if_missing
def grace_nvidia_grace_frontend_bound_cycles(grouped_df):
    stall_cycles_fe_series = grouped_df.get_group(
        "r23"  # STALL_CYCLES_FRONTEND
    ).counter_value
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    stall_cycles_fe_series.index = cycles_series.index
    return {
        "name": "NVIDIA GRACE FrontendBound %",
        "series": stall_cycles_fe_series.div(cycles_series),
        "prefix": 100,
    }


@skip_if_missing
def grace_nvidia_grace_backend_bound_cycles(grouped_df):
    stall_cycles_be_series = grouped_df.get_group(
        "r24"  # STALL_CYCLES_BACKEND
    ).counter_value
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    stall_cycles_be_series.index = cycles_series.index

    return {
        "name": "NVIDIA GRACE BackendBound %",
        "series": stall_cycles_be_series.div(cycles_series),
        "prefix": 100,
    }


@skip_if_missing
def grace_nvidia_grace_frontend_backend_boundness(grouped_df):
    stall_slot_series = grouped_df.get_group(
        "r3F"  # STALL_SLOTS
    ).counter_value
    br_mis_pred_series = grouped_df.get_group("r10").counter_value  # BR_MISPRED
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    stall_slot_series.index = cycles_series.index
    br_mis_pred_series.index = cycles_series.index

    bound_series = (
        stall_slot_series / (8 * cycles_series) - br_mis_pred_series * 4 / cycles_series
    )
    return {
        "name": "NVIDIA GRACE Frontend Backend Boundness %",
        "series": bound_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_bad_speculation(grouped_df):
    op_retired_series = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec_series = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot_series = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    br_mis_pred_series = grouped_df.get_group("r10").counter_value  # BR_MISPRED
    cycles_series = grouped_df.get_group("cpu_cycles").counter_value

    op_retired_series.index = cycles_series.index
    op_spec_series.index = cycles_series.index
    stall_slot_series.index = cycles_series.index
    br_mis_pred_series.index = cycles_series.index

    bad_speculation_series = (1 - op_retired_series / op_spec_series) * (
        1 - (stall_slot_series / (8 * cycles_series))
    ) + (br_mis_pred_series * 4 / cycles_series)

    return {
        "name": "TopDown Bad Speculation %",
        "series": bad_speculation_series,
        "prefix": 100,
    }


@skip_if_missing
def grace_nvidia_scf_mem_read_bw_MBps(grouped_df):
    cmem_rd_data_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_data/"
    ).counter_value
    duration_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_data/"
    ).counter_runtime
    pcnt_running_series = grouped_df.get_group("nvidia_scf_pmu_0/cmem_rd_data/").mux

    cmem_rd_data_series.index = duration_series.index
    pcnt_running_series.index = duration_series.index

    # Scale runtime by mux to get full measurement interval duration.
    # Counter values are auto-scaled by perf stat, but counter_runtime
    # reflects only the fraction of time the PMU was active.
    dt_series = duration_series * (100.0 / pcnt_running_series)
    local_mem_read_series = cmem_rd_data_series * 32
    local_mem_bw_read_series = local_mem_read_series.div(dt_series)
    return {
        "name": "SCF Local Memory Read Bandwidth (MBps)",
        "series": local_mem_bw_read_series,
        "prefix": 1000,
    }


@skip_if_missing
def grace_nvidia_scf_mem_write_bw_MBps(grouped_df):
    cmem_wr_bytes_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_wr_total_bytes/"
    ).counter_value
    duration_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_wr_total_bytes/"
    ).counter_runtime
    pcnt_running_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_wr_total_bytes/"
    ).mux

    cmem_wr_bytes_series.index = duration_series.index
    pcnt_running_series.index = duration_series.index

    dt_series = duration_series * (100.0 / pcnt_running_series)
    local_mem_bw_write_series = cmem_wr_bytes_series.div(dt_series)
    return {
        "name": "SCF Local Memory Write Bandwidth (MBps)",
        "series": local_mem_bw_write_series,
        "prefix": 1000,
    }


@skip_if_missing
def grace_nvidia_scf_mem_latency_ns(grouped_df):
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
    pcnt_running_series = grouped_df.get_group(
        "nvidia_scf_pmu_0/cmem_rd_outstanding/"
    ).mux

    cmem_rd_outstanding_series.index = sfc_cycles_series.index
    cmem_rd_access_series.index = sfc_cycles_series.index
    duration_series.index = sfc_cycles_series.index
    pcnt_running_series.index = sfc_cycles_series.index

    dt_series = duration_series * (100.0 / pcnt_running_series)
    local_mem_read_lat_ns_series = (
        cmem_rd_outstanding_series.div(cmem_rd_access_series)
    ) / (sfc_cycles_series.div(dt_series))
    return {
        "name": "SCF Local Memory Read Latency (nsecs)",
        "series": local_mem_read_lat_ns_series,
    }


# ===========================================================================
# NVIDIA Grace (Neoverse V2) -- core PMU + SCF uncore
# ===========================================================================


def metrics(grouped_df):
    return [
        grace_timestamp(grouped_df),
        grace_mips(grouped_df),
        grace_muopps(grouped_df),
        grace_ipc(grouped_df),
        grace_int_inst_percent(grouped_df),
        grace_simd_inst_percent(grouped_df),
        grace_fp_inst_percent(grouped_df),
        grace_ld_inst_percent(grouped_df),
        grace_st_inst_percent(grouped_df),
        grace_crypto_inst_percent(grouped_df),
        grace_branch_inst_percent(grouped_df),
        grace_gflops(grouped_df),
        grace_sve_gflops(grouped_df),
        grace_branch_mpki(grouped_df),
        grace_branch_miss_rate(grouped_df),
        grace_l1_icache_mpki(grouped_df),
        grace_l1_icache_miss_rate(grouped_df),
        grace_l1_dcache_mpki(grouped_df),
        grace_l1_dcache_miss_rate(grouped_df),
        grace_l2_cache_mpki(grouped_df),
        grace_l2_cache_code_mpki(grouped_df),
        grace_l2_cache_miss_rate(grouped_df),
        grace_l3_cache_mpki(grouped_df),
        grace_l3_cache_miss_rate(grouped_df),
        grace_itlb_mpki(grouped_df),
        grace_itlb_miss_rate(grouped_df),
        grace_dtlb_mpki(grouped_df),
        grace_dtlb_miss_rate(grouped_df),
        grace_l2tlb_mpki(grouped_df),
        grace_l2tlb_miss_rate(grouped_df),
        grace_itlb_walk_mpki(grouped_df),
        grace_dtlb_walk_mpki(grouped_df),
        grace_retiring_slots(grouped_df),
        grace_frontend_bound_slots(grouped_df),
        grace_backend_bound_slots(grouped_df),
        grace_nvidia_grace_frontend_bound_cycles(grouped_df),
        grace_nvidia_grace_backend_bound_cycles(grouped_df),
        grace_nvidia_grace_frontend_backend_boundness(grouped_df),
        grace_bad_speculation(grouped_df),
        grace_nvidia_scf_mem_read_bw_MBps(grouped_df),
        grace_nvidia_scf_mem_write_bw_MBps(grouped_df),
        grace_nvidia_scf_mem_latency_ns(grouped_df),
    ]


register_arch(
    "grace",
    metrics,
    vendor="arm",
    align="shortest",
    description="NVIDIA Grace (Neoverse V2) -- core PMU + SCF uncore",
)
