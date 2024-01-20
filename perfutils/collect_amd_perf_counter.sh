#!/bin/bash

SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do
  DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
  SOURCE="$(readlink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"
done
DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
ME="$(basename "$0")"
INTERVAL_SECS=5

# Disable NMI watchdog
sysctl kernel.nmi_watchdog=0 >/dev/null

UNHALTED_CYCLES_EV='cpu/event=0x76,umask=0,name=unhalted-cycles/'
STALLED_CYCLES_IDQ_EMPTY_EV='cpu/event=0x87,umask=0x02,name=stalled_cycles.idq_empty/'
STALLED_CYCLES_BACK_PRESSURE_EV='cpu/event=0x87,umask=0x01,name=stalled_cycles.back_pressure/'
STALLED_CYCLES_ANY_EV='cpu/event=0x87,umask=0x04,name=stalled_cycles.any/'

L1_IC_FETCHES_EV='cpu/event=0x80,umask=0,name=l1_ic_fetches/'
L1_IC_MISSES_EV='cpu/event=0x64,umask=0x07,name=l1_ic_misses/'
L1_DC_ACCESSES_EV='cpu/event=0x40,umask=0,name=l1_dc_accesses/'
L1_DC_MISSES_EV='cpu/event=0x41,umask=0x1f,name=l1_dc_misses/'

IC_FILL_L2_EV='cpu/event=0x82,umask=0,name=ic_cache_fill_l2/'
IC_FILL_SYS_EV='cpu/event=0x83,umask=0,name=ic_cache_fill_sys/'

IC_MAB_REQUESTS_EV='cpu/event=0x280,umask=0,name=ic_mab_requests_total/'
IC_MAB_REQUESTS_PREFETCH_EV='cpu/event=0x284,umask=0,name=ic_mab_requests_prefetch/'
IC_MAB_REQUESTS_DEMAND_EV='cpu/event=0x285,umask=0,name=ic_mab_requests_demand/'

MAB_ALLOC_CLKS_EV='cpu/event=0x5f,umask=0,name=mab_alloc_clks/'
MAB_PIPE_ALLOC_EV='cpu/event=0x41,umask=0x1f,name=mab_pipe_alloc/'

L2_IC_REQUESTS_G1_EV='cpu/event=0x60,umask=0x10,name=l2_ic_requests_g1/'
L2_IC_REQUESTS_G2_EV='cpu/event=0x61,umask=0x18,name=l2_ic_requests_g2/'
L2_DC_REQUESTS_EV='cpu/event=0x60,umask=0xc8,name=l2_dc_requests/'
L2_IC_HITS_EV='cpu/event=0x64,umask=0x06,name=l2_ic_hits/'
L2_DC_HITS_EV='cpu/event=0x64,umask=0x70,name=l2_dc_hits/'

L3_ACCESSES_EV='amd_l3/event=0x01,umask=0x80,name=l3_acceses/'
L3_MISSES_EV='amd_l3/event=0x06,umask=0x01,name=l3_misses/'
L3_FILL_RD_RESP_LAT_EV='amd_l3/event=0x90,umask=0x0,name=l3_fill_rd_resp_lat/'
L3_RD_RESP_CNT_EV='amd_l3/event=0x9A,umask=0x1F,name=l3_rd_resp_cnt/'
L3_FILL_LAT_OTHER_RD_RESP_EV='amd_l3/event=0x9B,umask=0x0B,name=l3_fill_lat_other_rd_resp/'

DF_GMI_CLK_CYCLES_EV='amd_df/event=0x780,umask=0x01,name=df_gmi_clock_cycles/'
DF_UMC_C_READ_REQS_EV='amd_df/event=0x87,umask=0x30,name=umc_c_read_requests/'
DF_UMC_C_CANCELS_ISSD_EV='amd_df/event=0x87,umask=0x02,name=umc_c_cancels_issued/'
DF_UMC_G_READ_REQS_EV='amd_df/event=0x107,umask=0x30,name=umc_g_read_requests/'
DF_UMC_G_CANCELS_ISSD_EV='amd_df/event=0x107,umask=0x02,name=umc_g_cancels_issued/'

DF_UMC_C_WRITE_REQS_EV='amd_df/event=0x87,umask=0x08,name=umc_c_write_requests/'
DF_UMC_D_WRITE_REQS_EV='amd_df/event=0xC7,umask=0x08,name=umc_d_write_requests/'
DF_UMC_G_WRITE_REQS_EV='amd_df/event=0x107,umask=0x08,name=umc_g_write_requests/'
DF_UMC_H_WRITE_REQS_EV='amd_df/event=0x147,umask=0x08,name=umc_h_write_requests/'

ITLB_MISSES_EV='cpu/event=0x84,umask=0,name=itlb_misses/'
L2_ITLB_MISSES_EV='cpu/event=0x85,umask=0x07,name=l2_itlb_misses/'
L2_ITLB_4K_MISSES_EV='cpu/event=0x85,umask=0x01,name=l2_4k_itlb_misses/'
L2_ITLB_2M_MISSES_EV='cpu/event=0x85,umask=0x02,name=l2_2m_itlb_misses/'
L2_ITLB_1G_MISSES_EV='cpu/event=0x85,umask=0x04,name=l2_1g_itlb_misses/'

DTLB_MISSES_EV='cpu/event=0x45,umask=0x0f,name=dtlb_misses/'
L1_DTLB_4K_MISSES_EV='cpu/event=0x45,umask=0x11,name=l1_4k_dtlb_misses/'
L1_DTLB_2M_MISSES_EV='cpu/event=0x45,umask=0x44,name=l1_2m_dtlb_misses/'
L1_DTLB_1G_MISSES_EV='cpu/event=0x45,umask=0x88,name=l1_1g_dtlb_misses/'
L2_DTLB_MISSES_EV='cpu/event=0x45,umask=0xf0,name=l2_dtlb_misses/'


LS_DISPATCH_EV='cpu/event=0x29,umask=0x03,name=ls_dispatch/'
RETIRED_UOPS_EV='cpu/event=0xC1,umask=0,name=retired_uops/'
RETIRED_BRANCH_INSTRUCTIONS_EV='cpu/event=0xc2,umask=0,name=retired_branch_instructions/'
RETIRED_BRANCH_MISPRED_EV='cpu/event=0xc3,umask=0,name=retired_branch_mispred/'
RETIRED_MICROCODED_EV='cpu/event=0x25,umask=0x0f,name=retired_microcoded_instructions/'

UOPS_DISPATCHED_OC_EV='cpu/event=0xaa,umask=0x2,name=de_uops_dispatch_opcache/'
UOPS_DISPATCHED_DE_EV='cpu/event=0xaa,umask=0x1,name=de_uops_dispatch_decoder/'

STALLED_CYCLES_GROUP="{${UNHALTED_CYCLES_EV},${STALLED_CYCLES_IDQ_EMPTY_EV},${STALLED_CYCLES_BACK_PRESSURE_EV},${STALLED_CYCLES_ANY_EV}}"
L1_ICACHE_GROUP="{${L1_IC_FETCHES_EV},${L1_IC_MISSES_EV}}"
L1_DCACHE_GROUP="{${L1_DC_ACCESSES_EV},${L1_DC_MISSES_EV}}"
L1_IC_MAB_GROUP="{${IC_MAB_REQUESTS_EV},${IC_MAB_REQUESTS_PREFETCH_EV},${IC_MAB_REQUESTS_DEMAND_EV},${LS_DISPATCH_EV}}"
L2_CACHE_GROUP="${L2_IC_REQUESTS_G1_EV},${L2_IC_REQUESTS_G2_EV},${L2_DC_REQUESTS_EV},${L2_IC_HITS_EV}"
L3_CACHE_GROUP="${L3_ACCESSES_EV},${L3_MISSES_EV},${UOPS_DISPATCHED_OC_EV},${UOPS_DISPATCHED_DE_EV},${L2_DC_HITS_EV}"
L3_AVG_RD_LAT_CLKS_GROUP="${L3_FILL_RD_RESP_LAT_EV},${L3_RD_RESP_CNT_EV},${L3_FILL_LAT_OTHER_RD_RESP_EV}"

SOCKET_READ_BW_GROUP="${DF_UMC_C_READ_REQS_EV},${DF_UMC_G_READ_REQS_EV},${DF_UMC_C_CANCELS_ISSD_EV},${DF_UMC_G_CANCELS_ISSD_EV}"
SOCKET_WRITE_BW_GROUP="${DF_UMC_C_WRITE_REQS_EV},${DF_UMC_D_WRITE_REQS_EV},${DF_UMC_G_WRITE_REQS_EV},${DF_UMC_H_WRITE_REQS_EV}"

INSTR_GROUP="${RETIRED_UOPS_EV},${RETIRED_BRANCH_INSTRUCTIONS_EV},${RETIRED_BRANCH_MISPRED_EV},${RETIRED_MICROCODED_EV}"

TLB_GROUP="{cycles,instructions,${ITLB_MISSES_EV},${DTLB_MISSES_EV}}"
L2_ITLB_MISSES_PAGE_GROUP="{${L2_ITLB_MISSES_EV},${L2_ITLB_4K_MISSES_EV},${L2_ITLB_2M_MISSES_EV},${L2_ITLB_1G_MISSES_EV}}"
L1_DTLB_MISSES_PAGE_GROUP="{${L2_DTLB_MISSES_EV},${L1_DTLB_4K_MISSES_EV},${L1_DTLB_2M_MISSES_EV},${L1_DTLB_1G_MISSES_EV}}"

IC_MAB_GROUP="${MAB_ALLOC_CLKS_EV},${MAB_PIPE_ALLOC_EV},${IC_FILL_L2_EV},${IC_FILL_SYS_EV}"

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

  events="${TLB_GROUP},${L1_DTLB_MISSES_PAGE_GROUP},${L2_ITLB_MISSES_PAGE_GROUP}"
  events="${events},${INSTR_GROUP},${IC_MAB_GROUP},${L1_ICACHE_GROUP},${L1_DCACHE_GROUP}"
  events="${events},${L1_IC_MAB_GROUP},${STALLED_CYCLES_GROUP},${L2_CACHE_GROUP}"
  events="${events},${L3_CACHE_GROUP},${L3_AVG_RD_LAT_CLKS_GROUP},${SOCKET_READ_BW_GROUP}"
  events="${events},${SOCKET_WRITE_BW_GROUP},${DF_GMI_CLK_CYCLES_EV}"

  perf_stat "${events}" "${interval_ms}"
}

collect_counters "$1" 2>/tmp/${ME}.err
