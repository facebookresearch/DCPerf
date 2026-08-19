#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Google Axion (Neoverse V2) metrics.

Core PMU only. Collect with collect_axion_neoversev2_perf_counters.sh. Cycles
counter is named ``cycles``. Axion aligns metrics to the longest series.
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
def axion_timestamp(grouped_df):
    ts_series = grouped_df.get_group("cycles").timestamp
    return {"name": "Timestamp_Secs", "series": ts_series}


@skip_if_missing
def axion_duration(grouped_df):
    duration_series = get_duration_series(grouped_df.get_group("duration_time"))
    return {
        "name": "Per-Sample Effective Sampling Duration (msecs)",
        "series": duration_series,
        "prefix": 10**-6,
    }


@skip_if_missing
def axion_mips(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = get_duration_series(grouped_df.get_group("instructions"))
    return {
        "name": "MIPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def axion_muopps(grouped_df):
    inst_series = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    duration_series = get_duration_series(grouped_df.get_group("r3A"))
    return {
        "name": "MuOPPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def axion_ipc(grouped_df):
    cycles_series = grouped_df.get_group("cycles").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    cycles_series.index = inst_series.index
    return {"name": "IPC", "series": inst_series.div(cycles_series)}


# --- Instruction Mix ---


@skip_if_missing
def axion_int_inst_percent(grouped_df):
    int_series = grouped_df.get_group("r73").counter_value  # DP_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    int_series.index = inst_series.index
    return {
        "name": "INT instruction %",
        "series": int_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_simd_inst_percent(grouped_df):
    simd_series = grouped_df.get_group("r74").counter_value  # ASE_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    simd_series.index = inst_series.index
    return {
        "name": "SIMD instruction %",
        "series": simd_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_fp_inst_percent(grouped_df):
    fp_series = grouped_df.get_group("r75").counter_value  # VFP_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    fp_series.index = inst_series.index
    return {
        "name": "FP instruction %",
        "series": fp_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_ld_inst_percent(grouped_df):
    ld_series = grouped_df.get_group("r70").counter_value  # LD_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    ld_series.index = inst_series.index
    return {
        "name": "Load instruction %",
        "series": ld_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_st_inst_percent(grouped_df):
    st_series = grouped_df.get_group("r71").counter_value  # ST_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    st_series.index = inst_series.index
    return {
        "name": "Store instruction %",
        "series": st_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_crypto_inst_percent(grouped_df):
    crypto_series = grouped_df.get_group("r77").counter_value  # CRYPTO_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    crypto_series.index = inst_series.index
    return {
        "name": "Crypto instruction %",
        "series": crypto_series / inst_series,
        "prefix": 100,
    }


@skip_if_missing
def axion_branch_inst_percent(grouped_df):
    br_imm = grouped_df.get_group("r78").counter_value  # BR_IMMED_SPEC
    br_ind = grouped_df.get_group("r7a").counter_value  # BR_INDIRECT_SPEC
    inst_series = grouped_df.get_group("instructions").counter_value
    br_imm.index = inst_series.index
    br_ind.index = inst_series.index
    return {
        "name": "Branch instruction %",
        "series": (br_imm + br_ind) / inst_series,
        "prefix": 100,
    }


# --- FP Operations ---


@skip_if_missing
def axion_gflops(grouped_df):
    fp_scale = grouped_df.get_group("r80C0").counter_value  # FP_SCALE_OPS_SPEC
    fp_fixed = grouped_df.get_group("r80C1").counter_value  # FP_FIXED_OPS_SPEC
    duration_series = get_duration_series(grouped_df.get_group("r80C0"))
    fp_scale.index = duration_series.index
    fp_fixed.index = duration_series.index
    return {
        "name": "GFLOPS (any precision incl SVE)",
        "series": (fp_fixed + fp_scale).div(duration_series) / 10**9,
    }


@skip_if_missing
def axion_sve_gflops(grouped_df):
    fp_fixed = grouped_df.get_group("r80C1").counter_value  # FP_FIXED_OPS_SPEC
    duration_series = get_duration_series(grouped_df.get_group("r80C1"))
    fp_fixed.index = duration_series.index
    return {
        "name": "SVE GFLOPS (any precision)",
        "series": fp_fixed.div(duration_series) / 10**9,
    }


@skip_if_missing
def axion_fp_hp_percent(grouped_df):
    hp = grouped_df.get_group("r8014").counter_value  # FP_HP_SPEC
    fp_scale = grouped_df.get_group("r80C0").counter_value
    fp_fixed = grouped_df.get_group("r80C1").counter_value
    hp.index = fp_scale.index
    fp_fixed.index = fp_scale.index
    total = fp_scale + fp_fixed
    return {
        "name": "FP Half-Precision %",
        "series": hp / total.replace(0, float("nan")),
        "prefix": 100,
    }


@skip_if_missing
def axion_fp_sp_percent(grouped_df):
    sp = grouped_df.get_group("r8018").counter_value  # FP_SP_SPEC
    fp_scale = grouped_df.get_group("r80C0").counter_value
    fp_fixed = grouped_df.get_group("r80C1").counter_value
    sp.index = fp_scale.index
    fp_fixed.index = fp_scale.index
    total = fp_scale + fp_fixed
    return {
        "name": "FP Single-Precision %",
        "series": sp / total.replace(0, float("nan")),
        "prefix": 100,
    }


@skip_if_missing
def axion_fp_dp_percent(grouped_df):
    dp = grouped_df.get_group("r801C").counter_value  # FP_DP_SPEC
    fp_scale = grouped_df.get_group("r80C0").counter_value
    fp_fixed = grouped_df.get_group("r80C1").counter_value
    dp.index = fp_scale.index
    fp_fixed.index = fp_scale.index
    total = fp_scale + fp_fixed
    return {
        "name": "FP Double-Precision %",
        "series": dp / total.replace(0, float("nan")),
        "prefix": 100,
    }


# --- Branch Metrics ---


@skip_if_missing
def axion_branch_mpki(grouped_df):
    br_miss = grouped_df.get_group("r22").counter_value  # BR_MIS_PRED_RETIRED
    inst = grouped_df.get_group("instructions").counter_value
    br_miss.index = inst.index
    return {"name": "Branch MPKI", "series": br_miss.div(inst / 1000.0)}


@skip_if_missing
def axion_branch_miss_rate(grouped_df):
    br_miss = grouped_df.get_group("r22").counter_value  # BR_MIS_PRED_RETIRED
    br_total = grouped_df.get_group("r21").counter_value  # BR_RETIRED
    br_miss.index = br_total.index
    return {"name": "Branch Miss Rate %", "series": br_miss / br_total, "prefix": 100}


# --- Cache Metrics ---
# LMISS = long-latency miss (pipeline stall). REFILL = all refills (incl prefetch-satisfied).


@skip_if_missing
def axion_l1_icache_mpki(grouped_df):
    miss = grouped_df.get_group("r4006").counter_value  # L1I_CACHE_LMISS
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L1 iCache MPKI (LMISS)", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_l1_icache_refill_mpki(grouped_df):
    refill = grouped_df.get_group("r01").counter_value  # L1I_CACHE_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    refill.index = inst.index
    return {"name": "L1 iCache MPKI (REFILL)", "series": refill.div(inst / 1000.0)}


@skip_if_missing
def axion_l1_icache_miss_rate(grouped_df):
    miss = grouped_df.get_group("r4006").counter_value  # L1I_CACHE_LMISS
    access = grouped_df.get_group("r14").counter_value  # L1I_CACHE
    miss.index = access.index
    return {
        "name": "L1 iCache Miss Rate % (LMISS)",
        "series": miss / access,
        "prefix": 100,
    }


@skip_if_missing
def axion_l1_dcache_mpki(grouped_df):
    miss = grouped_df.get_group("r39").counter_value  # L1D_CACHE_LMISS_RD
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L1 dCache MPKI (LMISS_RD)", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_l1_dcache_refill_mpki(grouped_df):
    refill = grouped_df.get_group("r03").counter_value  # L1D_CACHE_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    refill.index = inst.index
    return {"name": "L1 dCache MPKI (REFILL)", "series": refill.div(inst / 1000.0)}


@skip_if_missing
def axion_l1_dcache_miss_rate(grouped_df):
    miss = grouped_df.get_group("r39").counter_value  # L1D_CACHE_LMISS_RD
    access = grouped_df.get_group("r04").counter_value  # L1D_CACHE
    miss.index = access.index
    return {
        "name": "L1 dCache Miss Rate % (LMISS_RD)",
        "series": miss / access,
        "prefix": 100,
    }


@skip_if_missing
def axion_l2_cache_mpki(grouped_df):
    refill = grouped_df.get_group("r17").counter_value  # L2D_CACHE_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    refill.index = inst.index
    return {"name": "L2 Cache MPKI (REFILL)", "series": refill.div(inst / 1000.0)}


@skip_if_missing
def axion_l2_cache_lmiss_mpki(grouped_df):
    miss = grouped_df.get_group("r4009").counter_value  # L2D_CACHE_LMISS_RD
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L2 Cache MPKI (LMISS_RD)", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_l2_cache_miss_rate(grouped_df):
    refill = grouped_df.get_group("r17").counter_value  # L2D_CACHE_REFILL
    access = grouped_df.get_group("r16").counter_value  # L2D_CACHE
    refill.index = access.index
    return {
        "name": "L2 Cache Miss Rate % (REFILL)",
        "series": refill / access,
        "prefix": 100,
    }


@skip_if_missing
def axion_l2_cache_lmiss_rate(grouped_df):
    miss = grouped_df.get_group("r4009").counter_value  # L2D_CACHE_LMISS_RD
    access = grouped_df.get_group("r16").counter_value  # L2D_CACHE
    miss.index = access.index
    return {
        "name": "L2 Cache Miss Rate % (LMISS_RD)",
        "series": miss / access,
        "prefix": 100,
    }


# --- TLB Metrics ---


@skip_if_missing
def axion_itlb_mpki(grouped_df):
    miss = grouped_df.get_group("r02").counter_value  # L1I_TLB_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L1 iTLB MPKI", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_itlb_miss_rate(grouped_df):
    miss = grouped_df.get_group("r02").counter_value  # L1I_TLB_REFILL
    access = grouped_df.get_group("r26").counter_value  # L1I_TLB
    miss.index = access.index
    return {"name": "L1 iTLB Miss Rate %", "series": miss / access, "prefix": 100}


@skip_if_missing
def axion_dtlb_mpki(grouped_df):
    miss = grouped_df.get_group("r05").counter_value  # L1D_TLB_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L1 dTLB MPKI", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_dtlb_miss_rate(grouped_df):
    miss = grouped_df.get_group("r05").counter_value  # L1D_TLB_REFILL
    access = grouped_df.get_group("r25").counter_value  # L1D_TLB
    miss.index = access.index
    return {"name": "L1 dTLB Miss Rate %", "series": miss / access, "prefix": 100}


@skip_if_missing
def axion_l2tlb_mpki(grouped_df):
    miss = grouped_df.get_group("r2D").counter_value  # L2D_TLB_REFILL
    inst = grouped_df.get_group("instructions").counter_value
    miss.index = inst.index
    return {"name": "L2 TLB MPKI", "series": miss.div(inst / 1000.0)}


@skip_if_missing
def axion_l2tlb_miss_rate(grouped_df):
    miss = grouped_df.get_group("r2D").counter_value  # L2D_TLB_REFILL
    access = grouped_df.get_group("r2F").counter_value  # L2D_TLB
    miss.index = access.index
    return {"name": "L2 TLB Miss Rate %", "series": miss / access, "prefix": 100}


@skip_if_missing
def axion_itlb_walk_mpki(grouped_df):
    walk = grouped_df.get_group("r35").counter_value  # ITLB_WALK
    inst = grouped_df.get_group("instructions").counter_value
    walk.index = inst.index
    return {"name": "iTLB Walk MPKI", "series": walk.div(inst / 1000.0)}


@skip_if_missing
def axion_dtlb_walk_mpki(grouped_df):
    walk = grouped_df.get_group("r34").counter_value  # DTLB_WALK
    inst = grouped_df.get_group("instructions").counter_value
    walk.index = inst.index
    return {"name": "dTLB Walk MPKI", "series": walk.div(inst / 1000.0)}


# --- TopDown Metrics ---
# Neoverse V2 is 8-wide decode. Pipeline width = 8 slots/cycle.
PIPELINE_WIDTH = 8


@skip_if_missing
def axion_retiring_slots(grouped_df):
    op_retired = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    cycles = grouped_df.get_group("cycles").counter_value
    op_retired.index = cycles.index
    op_spec.index = cycles.index
    stall_slot.index = cycles.index
    retiring = (op_retired / op_spec) * (1 - (stall_slot / (PIPELINE_WIDTH * cycles)))
    return {"name": "TopDown Retiring %", "series": retiring, "prefix": 100}


@skip_if_missing
def axion_frontend_bound_slots(grouped_df):
    fe_stall_slot = grouped_df.get_group("r3E").counter_value  # STALL_SLOT_FRONTEND
    br_mis = grouped_df.get_group("r10").counter_value  # BR_MIS_PRED
    cycles = grouped_df.get_group("cycles").counter_value
    fe_stall_slot.index = cycles.index
    br_mis.index = cycles.index
    fe_bound = (fe_stall_slot / (PIPELINE_WIDTH * cycles)) - (br_mis / cycles)
    return {"name": "TopDown FrontendBound %", "series": fe_bound, "prefix": 100}


@skip_if_missing
def axion_backend_bound_slots(grouped_df):
    be_stall_slot = grouped_df.get_group("r3D").counter_value  # STALL_SLOT_BACKEND
    br_mis = grouped_df.get_group("r10").counter_value  # BR_MIS_PRED
    cycles = grouped_df.get_group("cycles").counter_value
    be_stall_slot.index = cycles.index
    br_mis.index = cycles.index
    be_bound = (be_stall_slot / (PIPELINE_WIDTH * cycles)) - (br_mis * 3 / cycles)
    return {"name": "TopDown BackendBound %", "series": be_bound, "prefix": 100}


@skip_if_missing
def axion_bad_speculation(grouped_df):
    op_retired = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    br_mis = grouped_df.get_group("r10").counter_value  # BR_MIS_PRED
    cycles = grouped_df.get_group("cycles").counter_value
    op_retired.index = cycles.index
    op_spec.index = cycles.index
    stall_slot.index = cycles.index
    br_mis.index = cycles.index
    bad_spec = (1 - op_retired / op_spec) * (
        1 - (stall_slot / (PIPELINE_WIDTH * cycles))
    ) + (br_mis * 4 / cycles)
    return {"name": "TopDown Bad Speculation %", "series": bad_spec, "prefix": 100}


@skip_if_missing
def axion_frontend_bound_cycles(grouped_df):
    fe_stall = grouped_df.get_group("r23").counter_value  # STALL_FRONTEND
    cycles = grouped_df.get_group("cycles").counter_value
    fe_stall.index = cycles.index
    return {
        "name": "FrontendBound (cycle-based) %",
        "series": fe_stall.div(cycles),
        "prefix": 100,
    }


@skip_if_missing
def axion_backend_bound_cycles(grouped_df):
    be_stall = grouped_df.get_group("r24").counter_value  # STALL_BACKEND
    cycles = grouped_df.get_group("cycles").counter_value
    be_stall.index = cycles.index
    return {
        "name": "BackendBound (cycle-based) %",
        "series": be_stall.div(cycles),
        "prefix": 100,
    }


@skip_if_missing
def axion_backend_bound_mem_percent(grouped_df):
    be_mem = grouped_df.get_group("r4005").counter_value  # STALL_BACKEND_MEM
    be_stall = grouped_df.get_group("r3D").counter_value  # STALL_SLOT_BACKEND
    be_mem.index = be_stall.index
    return {
        "name": "BackendBound Memory % (of BE stalls)",
        "series": be_mem / be_stall.replace(0, float("nan")),
        "prefix": 100,
    }


# --- SVE Predication ---


@skip_if_missing
def axion_sve_pred_full_percent(grouped_df):
    full = grouped_df.get_group("r8076").counter_value  # SVE_PRED_FULL_SPEC
    total = grouped_df.get_group("r8074").counter_value  # SVE_PRED_SPEC
    full.index = total.index
    return {
        "name": "SVE Predication Full %",
        "series": full / total.replace(0, float("nan")),
        "prefix": 100,
    }


@skip_if_missing
def axion_sve_pred_empty_percent(grouped_df):
    empty = grouped_df.get_group("r8075").counter_value  # SVE_PRED_EMPTY_SPEC
    total = grouped_df.get_group("r8074").counter_value  # SVE_PRED_SPEC
    empty.index = total.index
    return {
        "name": "SVE Predication Empty %",
        "series": empty / total.replace(0, float("nan")),
        "prefix": 100,
    }


# --- Memory ---


@skip_if_missing
def axion_mem_access_rd_wr_ratio(grouped_df):
    rd = grouped_df.get_group("r66").counter_value  # MEM_ACCESS_RD
    wr = grouped_df.get_group("r67").counter_value  # MEM_ACCESS_WR
    rd.index = wr.index
    return {
        "name": "Memory Read/Write Ratio",
        "series": rd / wr.replace(0, float("nan")),
    }


# ===========================================================================
# Google Axion (Neoverse V2) -- core PMU only
# ===========================================================================


def metrics(grouped_df):
    return [
        axion_timestamp(grouped_df),
        axion_mips(grouped_df),
        axion_muopps(grouped_df),
        axion_ipc(grouped_df),
        axion_int_inst_percent(grouped_df),
        axion_simd_inst_percent(grouped_df),
        axion_fp_inst_percent(grouped_df),
        axion_ld_inst_percent(grouped_df),
        axion_st_inst_percent(grouped_df),
        axion_crypto_inst_percent(grouped_df),
        axion_branch_inst_percent(grouped_df),
        axion_gflops(grouped_df),
        axion_sve_gflops(grouped_df),
        axion_fp_hp_percent(grouped_df),
        axion_fp_sp_percent(grouped_df),
        axion_fp_dp_percent(grouped_df),
        axion_branch_mpki(grouped_df),
        axion_branch_miss_rate(grouped_df),
        axion_l1_icache_mpki(grouped_df),
        axion_l1_icache_refill_mpki(grouped_df),
        axion_l1_icache_miss_rate(grouped_df),
        axion_l1_dcache_mpki(grouped_df),
        axion_l1_dcache_refill_mpki(grouped_df),
        axion_l1_dcache_miss_rate(grouped_df),
        axion_l2_cache_mpki(grouped_df),
        axion_l2_cache_lmiss_mpki(grouped_df),
        axion_l2_cache_miss_rate(grouped_df),
        axion_l2_cache_lmiss_rate(grouped_df),
        axion_itlb_mpki(grouped_df),
        axion_itlb_miss_rate(grouped_df),
        axion_dtlb_mpki(grouped_df),
        axion_dtlb_miss_rate(grouped_df),
        axion_l2tlb_mpki(grouped_df),
        axion_l2tlb_miss_rate(grouped_df),
        axion_itlb_walk_mpki(grouped_df),
        axion_dtlb_walk_mpki(grouped_df),
        axion_retiring_slots(grouped_df),
        axion_frontend_bound_slots(grouped_df),
        axion_backend_bound_slots(grouped_df),
        axion_bad_speculation(grouped_df),
        axion_frontend_bound_cycles(grouped_df),
        axion_backend_bound_cycles(grouped_df),
        axion_backend_bound_mem_percent(grouped_df),
        axion_sve_pred_full_percent(grouped_df),
        axion_sve_pred_empty_percent(grouped_df),
        axion_mem_access_rd_wr_ratio(grouped_df),
    ]


register_arch(
    "axion",
    metrics,
    vendor="arm",
    align="longest",
    description="Google Axion (Neoverse V2) -- core PMU only",
)
