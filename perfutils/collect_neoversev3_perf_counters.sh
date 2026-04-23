#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ME="$(basename "$0")"
### Sampling Duration per event group
INTERVAL_SECS=5

### ARM Neoverse V3 (armv9.2-a, pmuv3)
###
### The following performance event names are defined in the Linux kernel perf
### PMU event JSON files for Neoverse V3:
### https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/tools/perf/pmu-events/arch/arm64/arm/neoverse-v3
###
### Event codes are sourced from the ARM Architecture Reference Manual and the
### kernel's common-and-microarch.json for standard architectural events.
###
### This script assumes that Linux perf user-level tool includes such JSON.
### Tested on perf version 6.6+
### Requires Linux Kernel 6.6+

### For these measurements, we assume long-running (>60 secs), steady-state workloads
### as we take 1-second sample of each group of events iteratively.
### That means full-iteration of samples, will take DURATION_TIME_SECS * number of perf_stat calls.

### Core PMU Events ------------------------------
### - Supports up to 6 PMCs per core on the Core PMU
###
### Neoverse V3 key differences from Neoverse V2:
###   - Different interconnect from Neoverse V2; no uncore PMU events here
###   - Richer Top-Down stall breakdown (frontend/backend sub-categories)
###   - New dispatch stall events (IQ_SX, IQ_MX, IQ_LS, IQ_VX, MCQ)
###   - L2 CHI interconnect busy indicators (CBusy0-3, MT)
###   - Extended SVE predication events
###   - New memory alignment latency events

### Cycles and Instructions
# cpu_cycles (0x11), inst_retired (0x08)
INSTRUCTIONS_RATE='cycles,instructions,duration_time,task-clock,r08'

### Cache Effectiveness Metrics
# L1D: l1d_cache (0x04), l1d_cache_refill (0x03), l1d_cache_miss (0x8144), l1d_cache_lmiss_rd (0x39)
L1_DCACHE_MISSES='r04,r03,r8144,r39'
# L1I: l1i_cache (0x14), l1i_cache_refill (0x01), l1i_cache_lmiss (0x4006)
L1_ICACHE_MISSES='r14,r01,r4006'
# L2: l2d_cache (0x16), l2d_cache_refill (0x17), l2d_cache_wb (0x18), l2d_cache_rd (0x50), l2d_cache_wr (0x51), l2d_cache_miss (0x814C)
L2_CACHE_MISSES='r16,r17,r18,r50,r51,r814C'
# LL (L3/SLC): ll_cache_rd (0x36), ll_cache_miss_rd (0x37)
L3_CACHE_MISSES='r36,r37'

### Memory Accesses
# mem_access (0x13), bus_access (0x19), bus_access_rd (0x60), bus_access_wr (0x61)
MEM_ACCESSES='r13,r19,r60,r61'
# mem_access_rd (0x66), mem_access_wr (0x67), remote_access (0x31)
MEM_ACCESSES_EXT='r66,r67,r31'

### Branches
# br_retired (0x21), br_mis_pred_retired (0x22), br_immed_spec (0x78), br_indirect_spec (0x7a)
BRANCH_MISPREDS='r21,r22,r78,r7a'

### Arithmetic / Instruction Mix
# dp_spec (0x73, int), ase_spec (0x74, SIMD arith), vfp_spec (0x75, FP ops),
# fp_scale_ops_spec (0x80C0, scalable FP), fp_fixed_ops_spec (0x80C1, non-scalable FP), crypto_spec (0x77)
ARITHMETRIC_RATE='r73,r74,r75,r80C0,r80C1,r77'

### SIMD / SVE
# ase_inst_spec (0x8005, Advanced SIMD inst), sve_inst_spec (0x8006, SVE inst), simd_inst_spec (0x8004)
SIMD_RATE='r8005,r8006,r8004'

### FP Precision Mix (new in V3 event set)
# fp_hp_spec (0x8014, half-precision), fp_sp_spec (0x8018, single), fp_dp_spec (0x801C, double)
FP_PRECISION='r8014,r8018,r801C'

### Memory Operations
# ld_spec (0x70, load), st_spec (0x71, store), int_spec (0x8040, int arith)
MEM_OP_RATE='r70,r71,r8040'

### Top-Down Metrics — Level 1
# op_retired (0x3A), op_spec (0x3B), stall_slot (0x3F), stall_backend_mem (0x4005)
RETIRING='r3A,r3B,r3F,r4005'
# stall (0x3C), stall_slot_backend (0x3D), stall_slot_frontend (0x3E),
# br_mis_pred (0x10), stall_frontend (0x23), stall_backend (0x24)
FE_BE_BOUNDEDNESS='r3C,r3D,r3E,r10,r23,r24'

### Top-Down Metrics — Level 2 Frontend Breakdown (new in V3)
# stall_frontend_cpubound (0x8160), stall_frontend_membound (0x8158),
# stall_frontend_flow (0x8161), stall_frontend_flush (0x8162)
FE_BREAKDOWN='r8160,r8158,r8161,r8162'
# stall_frontend_l1i (0x8159), stall_frontend_mem (0x815B), stall_frontend_tlb (0x815C)
FE_BREAKDOWN_MEM='r8159,r815B,r815C'

### Top-Down Metrics — Level 2 Backend Breakdown (new in V3)
# stall_backend_cpubound (0x816A), stall_backend_membound (0x8164),
# stall_backend_busy (0x816B), stall_backend_ilock (0x816C)
BE_BREAKDOWN='r816A,r8164,r816B,r816C'
# stall_backend_l1d (0x8165), stall_backend_l2d (0x8166),
# stall_backend_tlb (0x8167), stall_backend_st (0x8168), stall_backend_rename (0x816D)
BE_BREAKDOWN_MEM='r8165,r8166,r8167,r8168,r816D'

### Dispatch Stall Breakdown (V3-specific microarch events)
# dispatch_stall_iq_sx (0x15C, simple int), dispatch_stall_iq_mx (0x15D, complex int),
# dispatch_stall_iq_ls (0x15E, load/store), dispatch_stall_iq_vx (0x15F, vector),
# dispatch_stall_mcq (0x160, commit queue)
DISPATCH_STALLS='r15C,r15D,r15E,r15F,r160'

### TLB Effectiveness Metrics
# l1d_tlb (0x25), l1d_tlb_refill (0x05)
L1D_TLB_MISSES='r25,r05'
# l1i_tlb (0x26), l1i_tlb_refill (0x02)
L1I_TLB_MISSES='r26,r02'
# l2d_tlb (0x2F), l2d_tlb_refill (0x2D)
L2_TLB_MISSES='r2F,r2D'
# dtlb_walk (0x34)
DTLB_WALKS='r34'
# itlb_walk (0x35)
ITLB_WALKS='r35'

### L2 CHI Interconnect Busy Indicators (V3-specific microarch events)
# l2_chi_cbusy0 (0x198), l2_chi_cbusy1 (0x199), l2_chi_cbusy2 (0x19A),
# l2_chi_cbusy3 (0x19B), l2_chi_cbusy_mt (0x19C)
L2_CHI_BUSY='r198,r199,r19A,r19B,r19C'

### SVE Predication (V3-specific extended events)
# sve_pred_spec (0x8074), sve_pred_empty_spec (0x8075),
# sve_pred_full_spec (0x8076), sve_pred_partial_spec (0x8077)
SVE_PRED='r8074,r8075,r8076,r8077'

### CMN-Cypress Uncore PMU Events (SLC / System Level Cache) ---------
### Auto-discover arm_cmn_N devices (one per chiplet on NV3).
### HN-S (Home Node with SLC) events provide true SLC miss rate,
### which is not observable from the core PMU on Neoverse V3.
CMN_SLC_EVENTS=""
for cmn_dev in $(find /sys/bus/event_source/devices/ -maxdepth 1 -name 'arm_cmn_*' -printf '%f\n' 2>/dev/null | sort); do
    CMN_SLC_EVENTS+="${cmn_dev}/hns_slc_sf_cache_access_all/,"
    CMN_SLC_EVENTS+="${cmn_dev}/hns_cache_miss_all/,"
    CMN_SLC_EVENTS+="${cmn_dev}/hns_cache_fill_all/,"
    CMN_SLC_EVENTS+="${cmn_dev}/hns_mc_reqs_local_all/,"
    CMN_SLC_EVENTS+="${cmn_dev}/hns_pocq_reqs_recvd_all/,"
done
CMN_SLC_EVENTS="${CMN_SLC_EVENTS%,}"

## Purposedly let CPU events multiplex, simplifies our report generation
## In production take proper care of handling multiplexing
CPU_GROUP_MUX="${INSTRUCTIONS_RATE},${L1_DCACHE_MISSES},${L1_ICACHE_MISSES},${L2_CACHE_MISSES},${L3_CACHE_MISSES}\
,${MEM_ACCESSES},${MEM_ACCESSES_EXT},${ARITHMETRIC_RATE},${SIMD_RATE},${FP_PRECISION},${MEM_OP_RATE},${BRANCH_MISPREDS}\
,${RETIRING},${FE_BE_BOUNDEDNESS},${FE_BREAKDOWN},${FE_BREAKDOWN_MEM},${BE_BREAKDOWN},${BE_BREAKDOWN_MEM}\
,${DISPATCH_STALLS},${L1D_TLB_MISSES},${L1I_TLB_MISSES},${L2_TLB_MISSES},${DTLB_WALKS},${ITLB_WALKS}\
,${L2_CHI_BUSY},${SVE_PRED}"

PERF_PID=
wrapup() {
  kill -INT "$PERF_PID"
}

trap wrapup SIGINT SIGTERM

perf_stat() {
  local ev="$1"
  local interval_ms="$2"
  perf stat $ev -x, -I "${interval_ms}" -a --log-fd 1 &
  PERF_PID="$!"
  wait "$PERF_PID"
}

collect_counters() {
  local interval="$1"
  local outfile="$2"
  if [[ -n "$1" ]] && [[ "$1" -gt 0 ]]; then
    interval="$1"
  else
    interval="$INTERVAL_SECS"
  fi
  interval_ms="$((interval * 1000))"
  # Core PMU events in a single multiplexed group, plus CMN uncore if available.
  events="-e ${CPU_GROUP_MUX}"
  if [[ -n "$CMN_SLC_EVENTS" ]]; then
    events+=" -e ${CMN_SLC_EVENTS}"
  fi
  if [[ -n "$outfile" ]]; then
    perf_stat "$events" "$interval_ms" > "$outfile"
  else
    perf_stat "$events" "$interval_ms"
  fi
}

# Usage: collect_neoversev3_perf_counters.sh [interval_or_outfile] [outfile]
# If $1 is a number, it is the sampling interval in seconds.
# If $1 is not a number (e.g. a filename), it is treated as the output file
# and the default interval is used.
ARG1="$1"
ARG2="$2"

if [[ -n "$ARG1" ]] && ! [[ "$ARG1" =~ ^[0-9]+$ ]]; then
  # $1 is a filename, not an interval
  OUTFILE="$ARG1"
  collect_counters "" "$OUTFILE" 2>/tmp/"${ME}".err
elif [[ -n "$ARG2" ]]; then
  # $1 is interval, $2 is output file
  collect_counters "$ARG1" "$ARG2" 2>/tmp/"${ME}".err
else
  # $1 is interval (or empty), output to stdout
  collect_counters "$ARG1" "" 2>/tmp/"${ME}".err
fi
