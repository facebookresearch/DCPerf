#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""ARM Neoverse V3 metrics.

Core PMU + CMN uncore, Top-Down L2. Collect with
collect_neoversev3_perf_counters.sh. Cycles counter is named ``cycles``.
"""

try:
    from cea.chips.benchpress.perfutils.arm_perf.core import (
        _align,
        _sum_cmn_event,
        get_duration_series,
        register_arch,
        skip_if_missing,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf.core import (  # pyre-ignore[21]
        _align,
        _sum_cmn_event,
        get_duration_series,
        register_arch,
        skip_if_missing,
    )


@skip_if_missing
def v3_timestamp(grouped_df):
    ts_series = grouped_df.get_group("cycles").timestamp
    return {"name": "Timestamp_Secs", "series": ts_series}


@skip_if_missing
def v3_duration(grouped_df):
    duration_series = get_duration_series(grouped_df.get_group("duration_time"))
    return {
        "name": "Per-Sample Effective Sampling Duration (msecs)",
        "series": duration_series,
        "prefix": 10**-6,
    }


@skip_if_missing
def v3_mips(grouped_df):
    inst_series = grouped_df.get_group("instructions").counter_value
    duration_series = get_duration_series(grouped_df.get_group("instructions"))
    return {
        "name": "MIPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def v3_muopps(grouped_df):
    inst_series = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    duration_series = get_duration_series(grouped_df.get_group("r3A"))
    return {
        "name": "MuOPPS",
        "series": inst_series.astype("float").div(duration_series),
        "prefix": 10**-6,
    }


@skip_if_missing
def v3_ipc(grouped_df):
    cycles_series = grouped_df.get_group("cycles").counter_value
    inst_series = grouped_df.get_group("instructions").counter_value
    cycles_series.index = inst_series.index
    return {"name": "IPC", "series": inst_series.div(cycles_series)}


# ===========================================================================
# Instruction mix
# ===========================================================================


@skip_if_missing
def v3_int_inst_percent(grouped_df):
    int_s, inst_s = _align(grouped_df, "r73", "r1B")  # DP_SPEC / INST_SPEC
    return {"name": "INT instruction %", "series": int_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_simd_inst_percent(grouped_df):
    simd_s, inst_s = _align(grouped_df, "r74", "r1B")  # ASE_SPEC / INST_SPEC
    return {"name": "SIMD instruction %", "series": simd_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_fp_inst_percent(grouped_df):
    fp_s, inst_s = _align(grouped_df, "r75", "r1B")  # VFP_SPEC / INST_SPEC
    return {"name": "FP instruction %", "series": fp_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_ld_inst_percent(grouped_df):
    ld_s, inst_s = _align(grouped_df, "r70", "r1B")  # LD_SPEC / INST_SPEC
    return {"name": "Load instruction %", "series": ld_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_st_inst_percent(grouped_df):
    st_s, inst_s = _align(grouped_df, "r71", "r1B")  # ST_SPEC / INST_SPEC
    return {"name": "Store instruction %", "series": st_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_crypto_inst_percent(grouped_df):
    cr_s, inst_s = _align(grouped_df, "r77", "r1B")  # CRYPTO_SPEC / INST_SPEC
    return {"name": "Crypto instruction %", "series": cr_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_branch_inst_percent(grouped_df):
    br_imm = grouped_df.get_group("r78").counter_value  # BR_IMMED_SPEC
    br_ind = grouped_df.get_group("r7a").counter_value  # BR_INDIRECT_SPEC
    inst_s = grouped_df.get_group("r1B").counter_value  # INST_SPEC
    br_imm.index = inst_s.index
    br_ind.index = inst_s.index
    return {
        "name": "Branch instruction %",
        "series": (br_imm + br_ind) / inst_s,
        "prefix": 100,
    }


@skip_if_missing
def v3_sve_inst_percent(grouped_df):
    sve_s, inst_s = _align(grouped_df, "r8006", "r1B")  # SVE_INST_SPEC / INST_SPEC
    return {"name": "SVE instruction %", "series": sve_s / inst_s, "prefix": 100}


@skip_if_missing
def v3_int_arith_inst_percent(grouped_df):
    int_s, inst_s = _align(grouped_df, "r8040", "r1B")  # INT_SPEC / INST_SPEC
    return {
        "name": "INT Arith instruction %",
        "series": int_s / inst_s,
        "prefix": 100,
    }


# ===========================================================================
# FP precision mix (V3-specific)
# ===========================================================================


@skip_if_missing
def v3_fp_hp_percent(grouped_df):
    """Half-precision FP ops as % of all speculated instructions."""
    hp_s, spec_s = _align(grouped_df, "r8014", "r1B")  # FP_HP_SPEC / INST_SPEC
    return {"name": "FP Half-Precision %", "series": hp_s / spec_s, "prefix": 100}


@skip_if_missing
def v3_fp_sp_percent(grouped_df):
    """Single-precision FP ops as % of all speculated instructions."""
    sp_s, spec_s = _align(grouped_df, "r8018", "r1B")  # FP_SP_SPEC / INST_SPEC
    return {"name": "FP Single-Precision %", "series": sp_s / spec_s, "prefix": 100}


@skip_if_missing
def v3_fp_dp_percent(grouped_df):
    """Double-precision FP ops as % of all speculated instructions."""
    dp_s, spec_s = _align(grouped_df, "r801C", "r1B")  # FP_DP_SPEC / INST_SPEC
    return {"name": "FP Double-Precision %", "series": dp_s / spec_s, "prefix": 100}


# ===========================================================================
# FLOPS
# ===========================================================================


@skip_if_missing
def v3_gflops(grouped_df):
    fp_scale = grouped_df.get_group("r80C0").counter_value  # FP_SCALE_OPS_SPEC
    fp_fixed = grouped_df.get_group("r80C1").counter_value  # FP_FIXED_OPS_SPEC
    dur = get_duration_series(grouped_df.get_group("r80C0"))
    fp_scale.index = dur.index
    fp_fixed.index = dur.index
    return {
        "name": "GFLOPS (any precision incl SVE)",
        "series": (fp_fixed + fp_scale).div(dur) / 10**9,
    }


@skip_if_missing
def v3_sve_gflops(grouped_df):
    fp_scale = grouped_df.get_group("r80C0").counter_value  # FP_SCALE_OPS_SPEC
    dur = get_duration_series(grouped_df.get_group("r80C0"))
    fp_scale.index = dur.index
    return {
        "name": "SVE GFLOPS (any precision)",
        "series": fp_scale.div(dur) / 10**9,
    }


# ===========================================================================
# Branch prediction
# ===========================================================================


@skip_if_missing
def v3_branch_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r22", "instructions")  # BR_MIS_PRED_RETIRED
    return {"name": "Branch MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_branch_miss_rate(grouped_df):
    miss_s, br_s = _align(grouped_df, "r22", "r21")  # BR_MIS_PRED_RETIRED / BR_RETIRED
    return {"name": "Branch Miss Rate %", "series": miss_s / br_s, "prefix": 100}


# ===========================================================================
# Cache hierarchy
# ===========================================================================


@skip_if_missing
def v3_l1_icache_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r01", "instructions")  # L1I_CACHE_REFILL
    return {"name": "L1 iCache MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l1_icache_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r01", "r14")  # L1I_CACHE_REFILL / L1I_CACHE
    return {"name": "L1 iCache Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_l1_dcache_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r03", "instructions")  # L1D_CACHE_REFILL
    return {"name": "L1 dCache MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l1_dcache_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r03", "r04")  # L1D_CACHE_REFILL / L1D_CACHE
    return {"name": "L1 dCache Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_l1_dcache_lmiss_mpki(grouped_df):
    """L1D long-latency read miss MPKI (misses that go beyond L1)."""
    miss_s, inst_s = _align(grouped_df, "r39", "instructions")  # L1D_CACHE_LMISS_RD
    return {"name": "L1 dCache Long-Miss MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l2_cache_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r17", "instructions")  # L2D_CACHE_REFILL
    return {"name": "L2 Cache MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l2_cache_miss_mpki(grouped_df):
    """L2 cache miss MPKI using V3 L2D_CACHE_MISS (0x814C).
    Replaces V2's L2 code miss (0x108) which was IMPDEF on V2."""
    miss_s, inst_s = _align(grouped_df, "r814C", "instructions")  # L2D_CACHE_MISS
    return {"name": "L2 Cache Miss MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l2_cache_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r17", "r16")  # L2D_CACHE_REFILL / L2D_CACHE
    return {"name": "L2 Cache Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_l3_cache_mpki(grouped_df):
    # Prefer CMN uncore events for true SLC MPKI (reads + writes).
    # Falls back to r37 (LL_CACHE_MISS_RD) which on V3 only measures
    # L2 read misses leaving the PE toward SLC/DRAM.
    try:
        miss_s = _sum_cmn_event(grouped_df, "hns_cache_miss_all")
        inst_s = grouped_df.get_group("instructions").counter_value
        miss_s.index = inst_s.index
        return {"name": "L3 Cache MPKI", "series": miss_s.div(inst_s / 1000.0)}
    except KeyError:
        miss_s, inst_s = _align(grouped_df, "r37", "instructions")
        return {"name": "L3 Cache MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l3_cache_miss_rate(grouped_df):
    # Use CMN HN-S uncore events for the real SLC miss rate.
    # hns_cache_miss_all / hns_slc_sf_cache_access_all gives total (read+write)
    # SLC miss rate across all chiplets.
    # Falls back to None if CMN events are not available (kernel without
    # CONFIG_ARM_CMN=y or firmware without CMNPMU ACPI table).
    try:
        access_s = _sum_cmn_event(grouped_df, "hns_slc_sf_cache_access_all")
        miss_s = _sum_cmn_event(grouped_df, "hns_cache_miss_all")
        return {
            "name": "L3 Cache Miss Rate %",
            "series": miss_s / access_s,
            "prefix": 100,
        }
    except KeyError:
        return None


# ===========================================================================
# TLB hierarchy
# ===========================================================================


@skip_if_missing
def v3_itlb_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r02", "instructions")  # L1I_TLB_REFILL
    return {"name": "L1 iTLB MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_itlb_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r02", "r26")  # L1I_TLB_REFILL / L1I_TLB
    return {"name": "L1 iTLB Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_dtlb_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r05", "instructions")  # L1D_TLB_REFILL
    return {"name": "L1 dTLB MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_dtlb_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r05", "r25")  # L1D_TLB_REFILL / L1D_TLB
    return {"name": "L1 dTLB Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_l2tlb_mpki(grouped_df):
    miss_s, inst_s = _align(grouped_df, "r2D", "instructions")  # L2D_TLB_REFILL
    return {"name": "L2 TLB MPKI", "series": miss_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_l2tlb_miss_rate(grouped_df):
    miss_s, acc_s = _align(grouped_df, "r2D", "r2F")  # L2D_TLB_REFILL / L2D_TLB
    return {"name": "L2 TLB Miss Rate %", "series": miss_s / acc_s, "prefix": 100}


@skip_if_missing
def v3_itlb_walk_mpki(grouped_df):
    walk_s, inst_s = _align(grouped_df, "r35", "instructions")  # ITLB_WALK
    return {"name": "iTLB Walk MPKI", "series": walk_s.div(inst_s / 1000.0)}


@skip_if_missing
def v3_dtlb_walk_mpki(grouped_df):
    walk_s, inst_s = _align(grouped_df, "r34", "instructions")  # DTLB_WALK
    return {"name": "dTLB Walk MPKI", "series": walk_s.div(inst_s / 1000.0)}


# ===========================================================================
# Top-Down Level 1 metrics
# ===========================================================================


@skip_if_missing
def v3_retiring_slots(grouped_df):
    op_ret = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    cycles = grouped_df.get_group("cycles").counter_value
    op_ret.index = cycles.index
    op_spec.index = cycles.index
    stall_slot.index = cycles.index
    # retiring = (1 - STALL_SLOT / (CPU_CYCLES * 10)) * (OP_RETIRED / OP_SPEC)
    retire_pct = (1 - stall_slot / (10 * cycles)) * (op_ret / op_spec)
    return {"name": "TopDown Retiring %", "series": retire_pct, "prefix": 100}


@skip_if_missing
def v3_frontend_bound_slots(grouped_df):
    fe_slot = grouped_df.get_group("r3E").counter_value  # STALL_SLOT_FRONTEND
    fe_flush = grouped_df.get_group("r8162").counter_value  # STALL_FRONTEND_FLUSH
    cycles = grouped_df.get_group("cycles").counter_value
    fe_slot.index = cycles.index
    fe_flush.index = cycles.index
    # V3: STALL_SLOT_FRONTEND / (10 * CPU_CYCLES) - STALL_FRONTEND_FLUSH / CPU_CYCLES
    fe_pct = (fe_slot / (10 * cycles)) - (fe_flush / cycles)
    return {"name": "TopDown FrontendBound %", "series": fe_pct, "prefix": 100}


@skip_if_missing
def v3_backend_bound_slots(grouped_df):
    # V3: backend_bound = STALL_SLOT_BACKEND / (10 * CPU_CYCLES)
    be_slot = grouped_df.get_group("r3D").counter_value  # STALL_SLOT_BACKEND
    cycles = grouped_df.get_group("cycles").counter_value
    be_slot.index = cycles.index
    be_pct = be_slot / (10 * cycles)
    return {"name": "TopDown BackendBound %", "series": be_pct, "prefix": 100}


@skip_if_missing
def v3_bad_speculation(grouped_df):
    op_ret = grouped_df.get_group("r3A").counter_value  # OP_RETIRED
    op_spec = grouped_df.get_group("r3B").counter_value  # OP_SPEC
    stall_slot = grouped_df.get_group("r3F").counter_value  # STALL_SLOT
    fe_flush = grouped_df.get_group("r8162").counter_value  # STALL_FRONTEND_FLUSH
    cycles = grouped_df.get_group("cycles").counter_value
    op_ret.index = cycles.index
    op_spec.index = cycles.index
    stall_slot.index = cycles.index
    fe_flush.index = cycles.index
    # V3: (1 - STALL_SLOT/(10*cycles)) * (1 - OP_RETIRED/OP_SPEC) + STALL_FRONTEND_FLUSH/cycles
    bad_spec = (1 - stall_slot / (10 * cycles)) * (
        1 - op_ret / op_spec
    ) + fe_flush / cycles
    return {"name": "TopDown Bad Speculation %", "series": bad_spec, "prefix": 100}


# Cycle-based frontend/backend (more accurate on ARM than slot-based)
@skip_if_missing
def v3_frontend_bound_cycles(grouped_df):
    fe_cyc, cycles = _align(grouped_df, "r23", "cycles")  # STALL_FRONTEND
    return {"name": "FrontendBound Cycles %", "series": fe_cyc / cycles, "prefix": 100}


@skip_if_missing
def v3_backend_bound_cycles(grouped_df):
    be_cyc, cycles = _align(grouped_df, "r24", "cycles")  # STALL_BACKEND
    return {"name": "BackendBound Cycles %", "series": be_cyc / cycles, "prefix": 100}


# ===========================================================================
# Top-Down Level 2 -- Frontend breakdown (V3-specific)
# ===========================================================================


@skip_if_missing
def v3_fe_core_bound(grouped_df):
    """Frontend stalls due to core (non-memory) constraints."""
    cpu_s, fe_s = _align(
        grouped_df, "r8160", "r23"
    )  # STALL_FRONTEND_CPUBOUND / STALL_FRONTEND
    return {"name": "FE CoreBound %", "series": cpu_s / fe_s, "prefix": 100}


@skip_if_missing
def v3_fe_mem_bound(grouped_df):
    """Frontend stalls due to instruction fetch memory latency."""
    mem_s, fe_s = _align(
        grouped_df, "r8158", "r23"
    )  # STALL_FRONTEND_MEMBOUND / STALL_FRONTEND
    return {"name": "FE MemBound %", "series": mem_s / fe_s, "prefix": 100}


@skip_if_missing
def v3_fe_flow_bound(grouped_df):
    """Frontend stalls waiting for branch prediction."""
    flow_s, cpu_s = _align(
        grouped_df, "r8161", "r8160"
    )  # STALL_FRONTEND_FLOW / STALL_FRONTEND_CPUBOUND
    return {"name": "FE FlowBound %", "series": flow_s / cpu_s, "prefix": 100}


@skip_if_missing
def v3_fe_flush_bound(grouped_df):
    """Frontend stalls recovering from pipeline flush."""
    flush_s, cpu_s = _align(
        grouped_df, "r8162", "r8160"
    )  # STALL_FRONTEND_FLUSH / STALL_FRONTEND_CPUBOUND
    return {"name": "FE FlushBound %", "series": flush_s / cpu_s, "prefix": 100}


@skip_if_missing
def v3_fe_cache_bound(grouped_df):
    """Frontend stalls on instruction cache (L1I + L2I)."""
    l1i_s = grouped_df.get_group("r8159").counter_value  # STALL_FRONTEND_L1I
    mem_s = grouped_df.get_group("r815B").counter_value  # STALL_FRONTEND_MEM
    membound_s = grouped_df.get_group("r8158").counter_value  # STALL_FRONTEND_MEMBOUND
    l1i_s.index = membound_s.index
    mem_s.index = membound_s.index
    return {
        "name": "FE CacheBound %",
        "series": (l1i_s + mem_s) / membound_s,
        "prefix": 100,
    }


@skip_if_missing
def v3_fe_tlb_bound(grouped_df):
    """Frontend stalls on instruction TLB."""
    tlb_s, membound_s = _align(
        grouped_df, "r815C", "r8158"
    )  # STALL_FRONTEND_TLB / STALL_FRONTEND_MEMBOUND
    return {"name": "FE TLBBound %", "series": tlb_s / membound_s, "prefix": 100}


# ===========================================================================
# Top-Down Level 2 -- Backend breakdown (V3-specific)
# ===========================================================================


@skip_if_missing
def v3_be_core_bound(grouped_df):
    """Backend stalls due to core (non-memory) constraints."""
    cpu_s, be_s = _align(
        grouped_df, "r816A", "r24"
    )  # STALL_BACKEND_CPUBOUND / STALL_BACKEND
    return {"name": "BE CoreBound %", "series": cpu_s / be_s, "prefix": 100}


@skip_if_missing
def v3_be_mem_bound(grouped_df):
    """Backend stalls due to memory subsystem."""
    mem_s, be_s = _align(
        grouped_df, "r8164", "r24"
    )  # STALL_BACKEND_MEMBOUND / STALL_BACKEND
    return {"name": "BE MemBound %", "series": mem_s / be_s, "prefix": 100}


@skip_if_missing
def v3_be_l1d_bound(grouped_df):
    """Backend stalls on L1D cache (of cache-bound stalls)."""
    l1d_s = grouped_df.get_group("r8165").counter_value  # STALL_BACKEND_L1D
    mem_s = grouped_df.get_group("r4005").counter_value  # STALL_BACKEND_MEM
    l1d_s.index = mem_s.index
    # Kernel: STALL_BACKEND_L1D / (STALL_BACKEND_L1D + STALL_BACKEND_MEM)
    return {"name": "BE L1D Bound %", "series": l1d_s / (l1d_s + mem_s), "prefix": 100}


@skip_if_missing
def v3_be_l2d_bound(grouped_df):
    """Backend stalls on L2D+ cache (of cache-bound stalls)."""
    l1d_s = grouped_df.get_group("r8165").counter_value  # STALL_BACKEND_L1D
    mem_s = grouped_df.get_group("r4005").counter_value  # STALL_BACKEND_MEM
    mem_s.index = l1d_s.index
    # Kernel: STALL_BACKEND_MEM / (STALL_BACKEND_L1D + STALL_BACKEND_MEM)
    return {"name": "BE L2D+ Bound %", "series": mem_s / (l1d_s + mem_s), "prefix": 100}


@skip_if_missing
def v3_be_tlb_bound(grouped_df):
    """Backend stalls on data TLB."""
    tlb_s, membound_s = _align(
        grouped_df, "r8167", "r8164"
    )  # STALL_BACKEND_TLB / STALL_BACKEND_MEMBOUND
    return {"name": "BE TLB Bound %", "series": tlb_s / membound_s, "prefix": 100}


@skip_if_missing
def v3_be_st_bound(grouped_df):
    """Backend stalls on stores not yet committed."""
    st_s, membound_s = _align(
        grouped_df, "r8168", "r8164"
    )  # STALL_BACKEND_ST / STALL_BACKEND_MEMBOUND
    return {"name": "BE Store Bound %", "series": st_s / membound_s, "prefix": 100}


@skip_if_missing
def v3_be_busy_bound(grouped_df):
    """Backend stalls due to full issue queues."""
    busy_s, be_s = _align(
        grouped_df, "r816B", "r24"
    )  # STALL_BACKEND_BUSY / STALL_BACKEND (per kernel metrics.json)
    return {"name": "BE Busy (IQ Full) %", "series": busy_s / be_s, "prefix": 100}


@skip_if_missing
def v3_be_ilock_bound(grouped_df):
    """Backend stalls due to input dependency."""
    ilock_s, be_s = _align(
        grouped_df, "r816C", "r24"
    )  # STALL_BACKEND_ILOCK / STALL_BACKEND
    return {"name": "BE ILock (Dep) %", "series": ilock_s / be_s, "prefix": 100}


@skip_if_missing
def v3_be_rename_bound(grouped_df):
    """Backend stalls due to rename register exhaustion."""
    ren_s, cpu_s = _align(
        grouped_df, "r816D", "r816A"
    )  # STALL_BACKEND_RENAME / STALL_BACKEND_CPUBOUND
    return {"name": "BE Rename %", "series": ren_s / cpu_s, "prefix": 100}


# ===========================================================================
# Dispatch stall breakdown (V3-specific microarch events)
# ===========================================================================


@skip_if_missing
def v3_dispatch_stall_iq_sx(grouped_df):
    """Dispatch stalls -- simple integer IQ full."""
    sx_s, cycles = _align(grouped_df, "r15C", "cycles")
    return {"name": "Dispatch Stall IQ_SX %", "series": sx_s / cycles, "prefix": 100}


@skip_if_missing
def v3_dispatch_stall_iq_mx(grouped_df):
    """Dispatch stalls -- complex integer IQ full."""
    mx_s, cycles = _align(grouped_df, "r15D", "cycles")
    return {"name": "Dispatch Stall IQ_MX %", "series": mx_s / cycles, "prefix": 100}


@skip_if_missing
def v3_dispatch_stall_iq_ls(grouped_df):
    """Dispatch stalls -- load/store IQ full."""
    ls_s, cycles = _align(grouped_df, "r15E", "cycles")
    return {"name": "Dispatch Stall IQ_LS %", "series": ls_s / cycles, "prefix": 100}


@skip_if_missing
def v3_dispatch_stall_iq_vx(grouped_df):
    """Dispatch stalls -- vector IQ full."""
    vx_s, cycles = _align(grouped_df, "r15F", "cycles")
    return {"name": "Dispatch Stall IQ_VX %", "series": vx_s / cycles, "prefix": 100}


@skip_if_missing
def v3_dispatch_stall_mcq(grouped_df):
    """Dispatch stalls -- commit queue full."""
    mcq_s, cycles = _align(grouped_df, "r160", "cycles")
    return {"name": "Dispatch Stall MCQ %", "series": mcq_s / cycles, "prefix": 100}


# ===========================================================================
# SVE predication effectiveness (V3-specific)
# ===========================================================================


@skip_if_missing
def v3_cmn_mem_read_bw_MBps(grouped_df):
    """Memory read bandwidth from CMN MC request counters.

    Each hns_mc_reqs_local_all is a cache-line (64B) request to the memory
    controller, analogous to Grace's SCF cmem_rd_data.
    """
    mc_reqs = _sum_cmn_event(grouped_df, "hns_mc_reqs_local_all")
    dur = get_duration_series(grouped_df.get_group("instructions"))
    mc_reqs.index = dur.index
    bw_series = (mc_reqs * 64).div(dur)
    return {
        "name": "CMN Memory Read Bandwidth (MBps)",
        "series": bw_series,
        "prefix": 10**-6,
    }


@skip_if_missing
def v3_sve_pred_empty_pct(grouped_df):
    """SVE predicated ops with no active lanes (wasted work)."""
    empty_s, pred_s = _align(
        grouped_df, "r8075", "r8074"
    )  # SVE_PRED_EMPTY_SPEC / SVE_PRED_SPEC
    return {"name": "SVE Pred Empty %", "series": empty_s / pred_s, "prefix": 100}


@skip_if_missing
def v3_sve_pred_full_pct(grouped_df):
    """SVE predicated ops with all lanes active (ideal)."""
    full_s, pred_s = _align(
        grouped_df, "r8076", "r8074"
    )  # SVE_PRED_FULL_SPEC / SVE_PRED_SPEC
    return {"name": "SVE Pred Full %", "series": full_s / pred_s, "prefix": 100}


@skip_if_missing
def v3_sve_pred_partial_pct(grouped_df):
    """SVE predicated ops with partial active lanes."""
    part_s, pred_s = _align(
        grouped_df, "r8077", "r8074"
    )  # SVE_PRED_PARTIAL_SPEC / SVE_PRED_SPEC
    return {"name": "SVE Pred Partial %", "series": part_s / pred_s, "prefix": 100}


# ===========================================================================
# ARM Neoverse V3 -- core PMU + CMN uncore, Top-Down L2
# ===========================================================================


def metrics(grouped_df):
    return [
        v3_timestamp(grouped_df),
        v3_mips(grouped_df),
        v3_muopps(grouped_df),
        v3_ipc(grouped_df),
        v3_int_inst_percent(grouped_df),
        v3_simd_inst_percent(grouped_df),
        v3_fp_inst_percent(grouped_df),
        v3_ld_inst_percent(grouped_df),
        v3_st_inst_percent(grouped_df),
        v3_crypto_inst_percent(grouped_df),
        v3_branch_inst_percent(grouped_df),
        v3_sve_inst_percent(grouped_df),
        v3_int_arith_inst_percent(grouped_df),
        v3_fp_hp_percent(grouped_df),
        v3_fp_sp_percent(grouped_df),
        v3_fp_dp_percent(grouped_df),
        v3_gflops(grouped_df),
        v3_sve_gflops(grouped_df),
        v3_branch_mpki(grouped_df),
        v3_branch_miss_rate(grouped_df),
        v3_l1_icache_mpki(grouped_df),
        v3_l1_icache_miss_rate(grouped_df),
        v3_l1_dcache_mpki(grouped_df),
        v3_l1_dcache_miss_rate(grouped_df),
        v3_l1_dcache_lmiss_mpki(grouped_df),
        v3_l2_cache_mpki(grouped_df),
        v3_l2_cache_miss_mpki(grouped_df),
        v3_l2_cache_miss_rate(grouped_df),
        v3_l3_cache_mpki(grouped_df),
        v3_l3_cache_miss_rate(grouped_df),
        v3_itlb_mpki(grouped_df),
        v3_itlb_miss_rate(grouped_df),
        v3_dtlb_mpki(grouped_df),
        v3_dtlb_miss_rate(grouped_df),
        v3_l2tlb_mpki(grouped_df),
        v3_l2tlb_miss_rate(grouped_df),
        v3_itlb_walk_mpki(grouped_df),
        v3_dtlb_walk_mpki(grouped_df),
        v3_retiring_slots(grouped_df),
        v3_frontend_bound_slots(grouped_df),
        v3_backend_bound_slots(grouped_df),
        v3_bad_speculation(grouped_df),
        v3_frontend_bound_cycles(grouped_df),
        v3_backend_bound_cycles(grouped_df),
        v3_fe_core_bound(grouped_df),
        v3_fe_mem_bound(grouped_df),
        v3_fe_flow_bound(grouped_df),
        v3_fe_flush_bound(grouped_df),
        v3_fe_cache_bound(grouped_df),
        v3_fe_tlb_bound(grouped_df),
        v3_be_core_bound(grouped_df),
        v3_be_mem_bound(grouped_df),
        v3_be_l1d_bound(grouped_df),
        v3_be_l2d_bound(grouped_df),
        v3_be_tlb_bound(grouped_df),
        v3_be_st_bound(grouped_df),
        v3_be_busy_bound(grouped_df),
        v3_be_ilock_bound(grouped_df),
        v3_be_rename_bound(grouped_df),
        v3_dispatch_stall_iq_sx(grouped_df),
        v3_dispatch_stall_iq_mx(grouped_df),
        v3_dispatch_stall_iq_ls(grouped_df),
        v3_dispatch_stall_iq_vx(grouped_df),
        v3_dispatch_stall_mcq(grouped_df),
        v3_sve_pred_empty_pct(grouped_df),
        v3_sve_pred_full_pct(grouped_df),
        v3_sve_pred_partial_pct(grouped_df),
        v3_cmn_mem_read_bw_MBps(grouped_df),
    ]


register_arch(
    "neoversev3",
    metrics,
    vendor="arm",
    align="shortest",
    description="ARM Neoverse V3 -- core PMU + CMN uncore, Top-Down L2",
)
