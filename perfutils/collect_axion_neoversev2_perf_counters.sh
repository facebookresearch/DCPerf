#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ME="$(basename "$0")"
### Sampling Duration per event group
INTERVAL_SECS=5

### Google Axion ARM Neoverse V2 (armv9-a, pmuv3)
###
### The following performance event codes are from:
### https://cloud.google.com/compute/docs/instances/arm-on-compute#pmu_events
###
### Supports up to 6 PMCs per core on the Core PMU.
### For long-running (>60 secs) steady-state workloads, we take periodic samples
### of each group of events. Full iteration takes INTERVAL_SECS * number of perf_stat calls.

### Group 1: Cycles and Instructions
# cycles, instructions, duration_time, task-clock, INST_RETIRED (r08)
INSTRUCTIONS_RATE='cycles,instructions,duration_time,task-clock,r08'

### Group 2: L1 Data Cache
# L1D_CACHE (access), L1D_CACHE_REFILL, L1D_CACHE_LMISS_RD (true long-latency miss)
L1_DCACHE='r04,r03,r39'

### Group 3: L1 Instruction Cache
# L1I_CACHE (access), L1I_CACHE_REFILL, L1I_CACHE_LMISS (true long-latency miss)
L1_ICACHE='r14,r01,r4006'

### Group 4: L2 Cache
# L2D_CACHE (access), L2D_CACHE_REFILL, L2D_CACHE_WB, L2D_CACHE_RD, L2D_CACHE_WR, L2D_CACHE_LMISS_RD
L2_CACHE='r16,r17,r18,r50,r51,r4009'

### Group 5: Memory and Bus
# MEM_ACCESS, BUS_ACCESS, BUS_ACCESS_RD, BUS_ACCESS_WR, MEM_ACCESS_RD, MEM_ACCESS_WR
MEM_ACCESSES='r13,r19,r60,r61,r66,r67'

### Group 6: Branches
# BR_RETIRED, BR_MIS_PRED_RETIRED, BR_IMMED_SPEC, BR_INDIRECT_SPEC, BR_RETURN_SPEC, BR_MIS_PRED
BRANCHES='r21,r22,r78,r7a,r79,r10'

### Group 7: Arithmetic / Instruction Mix
# DP_SPEC (int), ASE_SPEC (simd), VFP_SPEC (fp), CRYPTO_SPEC, LD_SPEC, ST_SPEC
INST_MIX='r73,r74,r75,r77,r70,r71'

### Group 8: FP Operations
# FP_SCALE_OPS_SPEC, FP_FIXED_OPS_SPEC, FP_HP_SPEC, FP_SP_SPEC, FP_DP_SPEC
FP_OPS='r80C0,r80C1,r8014,r8018,r801C'

### Group 9: SIMD / SVE
# ASE_INST_SPEC, SVE_INST_SPEC, SVE_PRED_SPEC, SVE_PRED_EMPTY_SPEC, SVE_PRED_FULL_SPEC
SIMD_SVE='r8005,r8006,r8074,r8075,r8076'

### Group 10: TopDown (Retiring + Speculation)
# OP_RETIRED, OP_SPEC, STALL_SLOT, STALL_BACKEND_MEM, STALL, INST_SPEC
TOPDOWN_RETIRING='r3A,r3B,r3F,r4005,r3C,r1B'

### Group 11: TopDown (Frontend/Backend Bound)
# STALL_SLOT_BACKEND, STALL_SLOT_FRONTEND, STALL_FRONTEND, STALL_BACKEND, BR_MIS_PRED, CNT_CYCLES
TOPDOWN_BOUND='r3D,r3E,r23,r24,r10,r4004'

### Group 12: L1 Data TLB
# L1D_TLB (access), L1D_TLB_REFILL, DTLB_WALK
L1D_TLB='r25,r05,r34'

### Group 13: L1 Instruction TLB
# L1I_TLB (access), L1I_TLB_REFILL, ITLB_WALK
L1I_TLB='r26,r02,r35'

### Group 14: L2 TLB
# L2D_TLB (access), L2D_TLB_REFILL, L2D_TLB_REFILL_RD, L2D_TLB_REFILL_WR
L2_TLB='r2F,r2D,r5C,r5D'

### Compose the full multiplexed CPU group
CPU_GROUP_MUX="${INSTRUCTIONS_RATE},${L1_DCACHE},${L1_ICACHE},${L2_CACHE}\
,${MEM_ACCESSES},${BRANCHES},${INST_MIX},${FP_OPS},${SIMD_SVE}\
,${TOPDOWN_RETIRING},${TOPDOWN_BOUND},${L1D_TLB},${L1I_TLB},${L2_TLB}"

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
  if [[ -n "$1" ]] && [[ "$1" -gt 0 ]]; then
    interval="$1"
  else
    interval="$INTERVAL_SECS"
  fi
  interval_ms="$((interval * 1000))"
  # Google Axion has no SCF/uncore PMU exposed to guests.
  # All events are core PMU only. We let perf multiplex across groups.
  events="-e ${CPU_GROUP_MUX}"
  perf_stat "$events" "$interval_ms"
}

collect_counters "$1" 2>/tmp/"${ME}".err
