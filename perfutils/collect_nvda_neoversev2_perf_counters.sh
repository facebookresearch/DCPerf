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
INSTRUCTIONS_RATE='cycles,instructions,duration_time'

### Cache Effectiveness Metrics
L1_CACHE_MISSES='l1d_cache_refill,l1i_cache_refill,l1i_cache,l1d_cache'
L2_CACHE_MISSES='l2d_cache_refill,l2d_cache'
L3_CACHE_MISSES='ll_cache_miss_rd,ll_cache_rd'

### Branches
BRANCH_MISPREDS='br_mis_pred_retired,br_retired'

### Floating-Point
FLOPS_RATE='fp_scale_ops_spec,fp_fixed_ops_spec'

### TopDown Metrics
RETIRING='op_retired,op_spec,stall_slot'
FE_BE_BOUNDEDNESS='stall_slot_backend,stall_slot_frontend,br_mis_pred'

### TLB Effectiveness Metrics
L1D_TLB_MISSES='l1d_tlb_refill'
L1I_TLB_MISSES='l1i_tlb_refill'
L2_TLB_MISSES='l2d_tlb_refill'
DTLB_WALKS='dtlb_walk'
ITLB_WALKS='itlb_walk'

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

SCF_LOCAL_MEM_GROUP="${SCF_CYCLES_EV}\
,${SCF_LOCAL_TOTAL_MEM_BW_RD_EV},${SCF_LOCAL_TOTAL_MEM_BW_WR_EV}\
,${SCF_LOCAL_TOTAL_MEM_BW_RD_UTIL_EV},${SCF_LOCAL_TOTAL_MEM_BW_WR_UTIL_EV}\
,${SCF_LOCAL_MEM_READ_LATENCY_EV}"

SCF_REMOTE_MEM_GROUP="${SCF_REMOTE_TOTAL_MEM_BW_RD_EV}\
,${SCF_REMOTE_TOTAL_MEM_BW_WR_EV},${SCF_REMOTE_MEM_READ_LATENCY_EV}"

## Purposedly let CPU events multiplex, simplifies our report generation
## In production take proper care of handling multiplexing
CPU_GROUP_MUX="${INSTRUCTIONS_RATE},${L1_CACHE_MISSES},${L2_CACHE_MISSES},${L3_CACHE_MISSES}\
,${BRANCH_MISPREDS},${FLOPS_RATE},${RETIRING},${FE_BE_BOUNDEDNESS},${L1D_TLB_MISSES}\
,${L1I_TLB_MISSES},${L2_TLB_MISSES},${DTLB_WALKS},${ITLB_WALKS}"

PERF_PID=
wrapup() {
  kill -INT "$PERF_PID"
}

trap wrapup SIGINT SIGTERM

perf_stat() {
  local ev="$1"
  local interval_ms="$2"
  perf stat -e "$ev" -x, -I "${interval_ms}" --per-socket -a --log-fd 1 &
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
  events="${CPU_GROUP_MUX},${SCF_LOCAL_MEM_GROUP},${SCF_REMOTE_MEM_GROUP}"
  perf_stat "$events" "$interval_ms"
}

collect_counters "$1" 2>/tmp/${ME}.err
