#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ME="$(basename "$0")"
### Sampling Duration per event group
INTERVAL_SECS=5

### NVIDIA's Grace ARM Neoverse V2 (armv9-a, pmuv3)
###
### The following performance events names are defined in
### https://github.com/ARM-software/data/blob/master/pmu/neoverse-v2.json
###
### This script assumes that Linux perf user-lever tool, includes such JSON.
### Tested on perf version 6.4.13
### Requires Linux Kernel 6.4+

### For these measurements, we assume long-running (>60 secs), steady-state workloads
### as we take 1-second sample of each group of events iteratively.
### That means full-iteration of samples, will take DURATION_TIME_SECS * number of perf_stat calls.

### Core PMU Events ------------------------------
### - Supports up to 6 PMCs per core on the Core PMU

### Cycles and Instructions
# r08: ins
INSTRUCTIONS_RATE='cycles,instructions,duration_time,task-clock,r08'

### Cache Effectiveness Metrics
# l1d access, l1d miss
L1_DCACHE_MISSES='r04,r03'
# l1i access, l1i miss, l1i refill
L1_ICACHE_MISSES='r14,r01,r4006'
# L2 access, L2 miss, L2 miss, L2 read access, L2 write access, L2 code miss
L2_CACHE_MISSES='r16,r17,r18,r50,r51,r108'
# l3 read access, l3 read miss
L3_CACHE_MISSES='r36,r37'

### Uncore Accesses
# mem access, bus access, bus access read, bus access write
MEM_ACCESSES='r13,r19,r60,r61'

### Branches
# branch inst, branch miss, BR_IMMED_SPEC, BR_INDIRECT_SPEC
BRANCH_MISPREDS='r21,r22,r78,r7a'

### Arithmetic
# dp_spec (inst mix -- int), ase_spec (inst mix -- ase), vfp_spec (inst mix -- fp ops),  fp_scale_ops_spec (inst mix -- fp), fp_fixed_ops_spec (inst mix -- fp), crypto
ARITHMETRIC_RATE='r73,r74,r75,r80C0,r80C1,r77'

### SIMD (including mem and arithmetics)
# ase_inst_spec (inst mix -- ase_inst), sve_inst_spec (inst mix -- sve_inst),
SIMD_RATE='r8005,r8006'

### mem ops
# ld_spec (inst mix -- load), st_spec (inst mix -- store)
MEM_OP_RATE='r70,r71'

### TopDown Metrics
# uops retired, op_spec (uops executed), stall slots, stall backend mem
RETIRING='r3A,r3B,r3F,r4005'
# stall cycles, backend stall slots, frontend stall slots, mispredicted branch, frontend stall cycles, backend stall cycles
FE_BE_BOUNDEDNESS='r3C,r3D,r3E,r10,r23,r24'

### TLB Effectiveness Metrics
# l1d_tlb access, l1d_tlb miss
L1D_TLB_MISSES='r25,r05'
# l1i_tlb access, l1i_tlb miss
L1I_TLB_MISSES='r26,r02'
# l2d_tlb access, l2d_tlb miss
L2_TLB_MISSES='r2F,r2D'
# dtlb_walk
DTLB_WALKS='r34'
# itlb_walk
ITLB_WALKS='r35'

### SCF PMU Events ---------------------------------
### - SCF, or Scalable Coherence Fabric, its akin to an uncore or DF.
###   SCF supports up to 8 PMC events


### SCF Cycles and Accounting
SCF_CYCLES_EV='nvidia_scf_pmu_0/cycles/'

### Memory Bandwidth and Latency
SCF_LOCAL_TOTAL_MEM_BW_RD_EV='nvidia_scf_pmu_0/cmem_rd_data/'
SCF_LOCAL_TOTAL_MEM_BW_WR_EV='nvidia_scf_pmu_0/cmem_wr_total_bytes/'
SCF_LOCAL_TOTAL_MEM_BW_RD_UTIL_EV='nvidia_scf_pmu_0/cmem_rd_access/'
SCF_LOCAL_TOTAL_MEM_BW_WR_UTIL_EV='nvidia_scf_pmu_0/cmem_wb_access/,nvidia_scf_pmu_0/cmem_wr_access/'
SCF_LOCAL_MEM_READ_LATENCY_EV='nvidia_scf_pmu_0/cmem_rd_outstanding/'

SCF_REMOTE_TOTAL_MEM_BW_RD_EV='nvidia_scf_pmu_0/remote_socket_rd_data/'
SCF_REMOTE_TOTAL_MEM_BW_WR_EV='nvidia_scf_pmu_0/remote_socket_wr_total_bytes/'
SCF_REMOTE_MEM_READ_LATENCY_EV='nvidia_scf_pmu_0/socket_1_rd_outstanding/,nvidia_scf_pmu_0/socket_1_rd_access/'

SCF_LOCAL_MEM_GROUP="'{${SCF_CYCLES_EV}\
,${SCF_LOCAL_TOTAL_MEM_BW_RD_EV},${SCF_LOCAL_TOTAL_MEM_BW_WR_EV}\
,${SCF_LOCAL_TOTAL_MEM_BW_RD_UTIL_EV},${SCF_LOCAL_TOTAL_MEM_BW_WR_UTIL_EV}\
,${SCF_LOCAL_MEM_READ_LATENCY_EV}}'"

SCF_REMOTE_MEM_GROUP="${SCF_REMOTE_TOTAL_MEM_BW_RD_EV}\
,${SCF_REMOTE_TOTAL_MEM_BW_WR_EV},${SCF_REMOTE_MEM_READ_LATENCY_EV}"

## Purposedly let CPU events multiplex, simplifies our report generation
## In production take proper care of handling multiplexing
CPU_GROUP_MUX="${INSTRUCTIONS_RATE},${L1_DCACHE_MISSES},${L1_ICACHE_MISSES},${L2_CACHE_MISSES},${L3_CACHE_MISSES}\
,${MEM_ACCESSES},${ARITHMETRIC_RATE},${SIMD_RATE},${MEM_OP_RATE},${BRANCH_MISPREDS},${RETIRING}\
,${FE_BE_BOUNDEDNESS},${L1D_TLB_MISSES},${L1I_TLB_MISSES},${L2_TLB_MISSES},${DTLB_WALKS},${ITLB_WALKS}"

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
  # Separate core and memory PMU events to avoid scaling issues
  # reported in previous analyses. This ensures that the core
  # events and memory events are collected independently
  #
  # SCF_REMOTE_MEM_GROUP is removed because:
  # (1) It cannot co-exist with local memory events due to potential
  #     conflicts in event collection.
  # (2) We are using a single socket system, making remote memory
  #     events irrelevant as they would always report zero activity.
  events="-e ${CPU_GROUP_MUX} -e ${SCF_LOCAL_MEM_GROUP}"
  perf_stat "$events" "$interval_ms"
}

collect_counters "$1" 2>/tmp/"${ME}".err
