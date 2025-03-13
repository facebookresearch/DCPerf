#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do
  DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
  SOURCE="$(writelink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"
done
DIR="$( cd -P "$( dirname "$SOURCE" )" && pwd )"
ME="$(basename "$0")"
INTERVAL_SECS=5

# Disable NMI watchdog
sysctl kernel.nmi_watchdog=0 >/dev/null


# Common Core Events
L3_LOOKUP_STATE="-e '{l3_lookup_state.l3_miss,l3_lookup_state.all_coherent_accesses_to_l3}'"
LS_ANY_FILLS_FROM_SYS="-e '{ls_any_fills_from_sys.all,ls_any_fills_from_sys.local_l2}'"
LS_ANY_FILLS_FROM_SYS_ADDITIONAL="-e ls_any_fills_from_sys.local_ccx,ls_any_fills_from_sys.dram_io_all,ls_any_fills_from_sys.dram_io_near,ls_any_fills_from_sys.dram_io_far,ls_any_fills_from_sys.near_cache,ls_any_fills_from_sys.remote_cache"
LS_L1_D_TLB_MISS_ALL="-e '{ls_l1_d_tlb_miss.all,ls_l1_d_tlb_miss.all_l2_miss,ls_l1_d_tlb_miss.tlb_reload_coalesced_page_miss,ls_l1_d_tlb_miss.tlb_reload_2m_l2_miss,ls_l1_d_tlb_miss.tlb_reload_4k_l2_miss,ls_l1_d_tlb_miss.tlb_reload_1g_l2_miss}'"
LS_L1_D_TLB_MISS_HITS="-e '{ls_l1_d_tlb_miss.tlb_reload_4k_l2_hit,ls_l1_d_tlb_miss.tlb_reload_coalesced_page_hit,ls_l1_d_tlb_miss.tlb_reload_2m_l2_hit,ls_l1_d_tlb_miss.tlb_reload_1g_l2_hit}'"
BP_L1_TLB_MISS_L2_TLB="-e '{bp_l1_tlb_miss_l2_tlb_hit,bp_l1_tlb_miss_l2_tlb_miss.all,bp_l1_tlb_miss_l2_tlb_miss.if4k,bp_l1_tlb_miss_l2_tlb_miss.if2m,bp_l1_tlb_miss_l2_tlb_miss.if1g,bp_l1_tlb_miss_l2_tlb_miss.coalesced_4k}'"
IC_ANY_FILLS_FROM_SYS="-e '{cpu/event=0x29c,umask=0xdf,name=ic_any_fills_from_sys.all/,cpu/event=0x29c,umask=0x01,name=ic_any_fills_from_sys.local_l2/,cpu/event=0x29c,umask=0xde,name=ic_any_fills_from_sys.local_l2_miss/,cpu/event=0x29c,umask=0x02,name=ic_any_fills_from_sys.local_ccx/,cpu/event=0x29c,umask=0x14,name=ic_any_fills_from_sys.remote_cache/,cpu/event=0x29c,umask=0x48,name=ic_any_fills_from_sys.dram_io/}'"
EX_RET_BRN="-e '{ex_ret_brn,ex_ret_brn_misp}'"
LS_HW_PF_DC_FILLS="-e '{ls_hw_pf_dc_fills.all,cpu/event=0x5a,umask=0x14,name=ls_hw_pf_dc_fills.remote_cache/}'"
LS_DMND_FILLS_FROM_SYS_ALL="-e ls_dmnd_fills_from_sys.all"
EX_RET_UCODE_INSTR="-e ex_ret_ucode_instr"
EX_RET_UCODE_OPS="-e '{ex_ret_ucode_ops,cpu/ex_ret_ops,name=ex_ret_ops_ucpercent/}'"
EX_RET_BRN_TKN="-e ex_ret_brn_tkn,ex_ret_brn_ind_misp,ex_ret_near_ret_mispred,ex_ret_ind_brch_instr,ex_ret_cond,cpu/event=0x1c7,name=ex_ret_cond_misp/"
EX_RET_NEAR_RET="-e '{ex_ret_near_ret,ex_ret_uncond_brnch_instr}'"
L3_XI_SAMPLED_LATENCY="-e '{amd_l3/event=0xac,umask=0x3f,name=l3_xi_sampled_latency.all,enallcores=0x1,enallslices=0x1,sliceid=0x3,threadmask=0x3/,amd_l3/event=0xad,umask=0x3f,name=l3_xi_sampled_latency_requests.all,enallcores=0x1,enallslices=0x1,sliceid=0x3,threadmask=0x3/}'"
DE_OP_QUEUE_EMPTY="-e cpu/event=0xa9,umask=0x01,name=de_op_queue_empty/"
INSTRUCTIONS_U_KH="-e '{instructions:u,instructions:kh}'"
LS_INT_TAKEN="-e ls_int_taken"
EX_RET_MMX_FP_INSTR="-e '{cpu/event=0x0cb,umask=0x04,name=ex_ret_mmx_fp_instr.sse/,cpu/event=0x0cb,umask=0x07,name=ex_ret_mmx_fp_instr.all/}'"
LS_DISPATCH_LD_STORE="-e ls_dispatch.ld_dispatch,ls_dispatch.store_dispatch"
LS_NOT_HALTED_CYC_BACKEND_STALLS="-e '{cpu/ls_not_halted_cyc,name=ls_not_halted_cyc_backend/,de_no_dispatch_per_slot.backend_stalls,ex_no_retire.load_not_complete,ex_no_retire.not_complete,de_no_dispatch_per_slot.smt_contention}'"
R1F25_U_KH="-e '{r1f25:u,r1f25:kh}'"

COMMON_CORE_EVENTS="${L3_LOOKUP_STATE} ${LS_ANY_FILLS_FROM_SYS} ${LS_ANY_FILLS_FROM_SYS_ADDITIONAL} ${LS_L1_D_TLB_MISS_ALL} ${LS_L1_D_TLB_MISS_HITS} ${BP_L1_TLB_MISS_L2_TLB} ${IC_ANY_FILLS_FROM_SYS} ${EX_RET_BRN} ${LS_HW_PF_DC_FILLS} ${LS_DMND_FILLS_FROM_SYS_ALL} ${EX_RET_UCODE_INSTR} ${EX_RET_UCODE_OPS} ${EX_RET_BRN_TKN} ${EX_RET_NEAR_RET} ${L3_XI_SAMPLED_LATENCY} ${DE_OP_QUEUE_EMPTY} ${INSTRUCTIONS_U_KH} ${LS_INT_TAKEN} ${EX_RET_MMX_FP_INSTR} ${LS_DISPATCH_LD_STORE} ${LS_NOT_HALTED_CYC_BACKEND_STALLS} ${R1F25_U_KH}"

# Common DF Events
COMMON_DF_EVENTS='-e amd_df/event=0x00c,umask=0x2b7,name=cs0_probes_sent/'


# ZEN5 DMA events
AMD_DF_EVENT_0X81F_UMASK_0XFFE_NAME_IOM0_UPSTREAM_READ_BEATS='amd_df/event=0x81f,umask=0xffe,name=iom0_upstream_read_beats/'
AMD_DF_EVENT_0X85F_UMASK_0XFFE_NAME_IOM1_UPSTREAM_READ_BEATS='amd_df/event=0x85f,umask=0xffe,name=iom1_upstream_read_beats/'
AMD_DF_EVENT_0X89F_UMASK_0XFFE_NAME_IOM2_UPSTREAM_READ_BEATS='amd_df/event=0x89f,umask=0xffe,name=iom2_upstream_read_beats/'
AMD_DF_EVENT_0X8DF_UMASK_0XFFE_NAME_IOM3_UPSTREAM_READ_BEATS='amd_df/event=0x8df,umask=0xffe,name=iom3_upstream_read_beats/'
AMD_DF_EVENT_0X91F_UMASK_0XFFE_NAME_IOM4_UPSTREAM_READ_BEATS='amd_df/event=0x91f,umask=0xffe,name=iom4_upstream_read_beats/'
AMD_DF_EVENT_0X95F_UMASK_0XFFE_NAME_IOM5_UPSTREAM_READ_BEATS='amd_df/event=0x95f,umask=0xffe,name=iom5_upstream_read_beats/'
AMD_DF_EVENT_0X99F_UMASK_0XFFE_NAME_IOM6_UPSTREAM_READ_BEATS='amd_df/event=0x99f,umask=0xffe,name=iom6_upstream_read_beats/'
AMD_DF_EVENT_0X9DF_UMASK_0XFFE_NAME_IOM7_UPSTREAM_READ_BEATS='amd_df/event=0x9df,umask=0xffe,name=iom7_upstream_read_beats/'

AMD_DF_EVENT_0X81F_UMASK_0X7FF_NAME_IOM0_UPSTREAM_WRITE_BEATS='amd_df/event=0x81f,umask=0x7ff,name=iom0_upstream_write_beats/'
AMD_DF_EVENT_0X85F_UMASK_0X7FF_NAME_IOM1_UPSTREAM_WRITE_BEATS='amd_df/event=0x85f,umask=0x7ff,name=iom1_upstream_write_beats/'
AMD_DF_EVENT_0X89F_UMASK_0X7FF_NAME_IOM2_UPSTREAM_WRITE_BEATS='amd_df/event=0x89f,umask=0x7ff,name=iom2_upstream_write_beats/'
AMD_DF_EVENT_0X8DF_UMASK_0X7FF_NAME_IOM3_UPSTREAM_WRITE_BEATS='amd_df/event=0x8df,umask=0x7ff,name=iom3_upstream_write_beats/'
AMD_DF_EVENT_0X91F_UMASK_0X7FF_NAME_IOM4_UPSTREAM_WRITE_BEATS='amd_df/event=0x91f,umask=0x7ff,name=iom4_upstream_write_beats/'
AMD_DF_EVENT_0X95F_UMASK_0X7FF_NAME_IOM5_UPSTREAM_WRITE_BEATS='amd_df/event=0x95f,umask=0x7ff,name=iom5_upstream_write_beats/'
AMD_DF_EVENT_0X99F_UMASK_0X7FF_NAME_IOM6_UPSTREAM_WRITE_BEATS='amd_df/event=0x99f,umask=0x7ff,name=iom6_upstream_write_beats/'
AMD_DF_EVENT_0X9DF_UMASK_0X7FF_NAME_IOM7_UPSTREAM_WRITE_BEATS='amd_df/event=0x9df,umask=0x7ff,name=iom7_upstream_write_beats/'

ZEN5_DMA="-e '{${AMD_DF_EVENT_0X81F_UMASK_0XFFE_NAME_IOM0_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X85F_UMASK_0XFFE_NAME_IOM1_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X89F_UMASK_0XFFE_NAME_IOM2_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X8DF_UMASK_0XFFE_NAME_IOM3_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X91F_UMASK_0XFFE_NAME_IOM4_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X95F_UMASK_0XFFE_NAME_IOM5_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X99F_UMASK_0XFFE_NAME_IOM6_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X9DF_UMASK_0XFFE_NAME_IOM7_UPSTREAM_READ_BEATS},${AMD_DF_EVENT_0X81F_UMASK_0X7FF_NAME_IOM0_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X85F_UMASK_0X7FF_NAME_IOM1_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X89F_UMASK_0X7FF_NAME_IOM2_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X8DF_UMASK_0X7FF_NAME_IOM3_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X91F_UMASK_0X7FF_NAME_IOM4_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X95F_UMASK_0X7FF_NAME_IOM5_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X99F_UMASK_0X7FF_NAME_IOM6_UPSTREAM_WRITE_BEATS},${AMD_DF_EVENT_0X9DF_UMASK_0X7FF_NAME_IOM7_UPSTREAM_WRITE_BEATS}}'"

# ZEN5_CSBW
AMD_DF_EVENT_0X01F_UMASK_0XFFE_NAME_CS0_DRAM_READ_BEATS='amd_df/event=0x01f,umask=0xffe,name=cs0_dram_read_beats/'
AMD_DF_EVENT_0X01F_UMASK_0XFFF_NAME_CS0_DRAM_WRITE_BEATS='amd_df/event=0x01f,umask=0xfff,name=cs0_dram_write_beats/'


ZEN5_CSBW="-e '{${AMD_DF_EVENT_0X01F_UMASK_0XFFE_NAME_CS0_DRAM_READ_BEATS},${AMD_DF_EVENT_0X01F_UMASK_0XFFF_NAME_CS0_DRAM_WRITE_BEATS}}'"

# ZEN5 EVENTS
DE_DISPATCH_STALL_CYCLE_DYNAMIC_TOKENS="-e '{cpu/event=0xae,umask=0x57,name=de_dispatch_stall_cycle_dynamic_tokens_part1.all/,cpu/event=0xaf,umask=0x27,name=de_dispatch_stall_cycle_dynamic_tokens_part2.all/}'"
FRONTEND_LATENCY="-e '{cpu/de_no_dispatch_per_slot.no_ops_from_frontend,name=frontend_latency,cmask=0x8/,de_no_dispatch_per_slot.no_ops_from_frontend,ls_not_halted_cyc,de_src_op_disp.all,ex_ret_ops}'"
DE_SRC_OP_DISP_X86_DECODER_OP_CACHE="-e '{de_src_op_disp.x86_decoder,de_src_op_disp.op_cache}'"
L2_PF_MISS_L2_HIT_L3="-e '{l2_pf_miss_l2_hit_l3.l2_hwpf,l2_pf_miss_l2_l3.l2_hwpf,l2_cache_req_stat.ic_dc_miss_in_l2,l2_cache_req_stat.ic_fill_miss}'"
DE_DIS_OPS_FROM_DECODER_FP_INTEGER_DISPATCH="-e '{de_dis_ops_from_decoder.any_fp_dispatch,de_dis_ops_from_decoder.any_integer_dispatch}'"
CS_CMP_CXL="-e '{amd_df/event=0x31f,umask=0xffe,name=cs_cmp0_cxl_read_beats/,amd_df/event=0x35f,umask=0xffe,name=cs_cmp1_cxl_read_beats/,amd_df/event=0x39f,umask=0xffe,name=cs_cmp2_cxl_read_beats/,amd_df/event=0x3df,umask=0xffe,name=cs_cmp3_cxl_read_beats/,amd_df/event=0x31f,umask=0xcfd,name=cs_cmp0_cxl_write_beats/,amd_df/event=0x35f,umask=0xcfd,name=cs_cmp1_cxl_write_beats/,amd_df/event=0x39f,umask=0xcfd,name=cs_cmp2_cxl_write_beats/,amd_df/event=0x3df,umask=0xcfd,name=cs_cmp3_cxl_write_beats/}'"


amd_umc_device_dirs=$(ls -d /sys/bus/event_source/devices/amd_umc_* 2>/dev/null)
#if there are any, otherwise just set to ""
if [ -n "$amd_umc_device_dirs" ]; then
  amd_umc_devices=$(echo $amd_umc_device_dirs | xargs -n 1 basename | awk -F'_' '{print $3}' | sort -V)
  ZEN5_UMC=""
  for n in $amd_umc_devices; do
    ZEN5_UMC+="-e '{amd_umc_$n/event=0,name=umc_cyc_umc$n/,amd_umc_$n/event=0x14,rdwrmask=0,name=umc_data_cyc_umc$n/}' "
  done
  ZEN5_UMC=${ZEN5_UMC%,}
else
  ZEN5_UMC=""
fi


ZEN5_EVENTS="${DE_DISPATCH_STALL_CYCLE_DYNAMIC_TOKENS} ${FRONTEND_LATENCY} ${DE_SRC_OP_DISP_X86_DECODER_OP_CACHE} ${L2_PF_MISS_L2_HIT_L3} ${DE_DIS_OPS_FROM_DECODER_FP_INTEGER_DISPATCH} ${CS_CMP_CXL}"

# ZEN5ES EVENTS

CS_CMP_CXL_WRITE_BEATS="-e '{amd_df/event=0x31f,umask=0xcfd,name=cs_cmp0_cxl_write_beats/,amd_df/event=0x35f,umask=0xcfd,name=cs_cmp1_cxl_write_beats/,amd_df/event=0x39f,umask=0xcfd,name=cs_cmp2_cxl_write_beats/,amd_df/event=0x3df,umask=0xcfd,name=cs_cmp3_cxl_write_beats/}'"
CCM_CXL_READ_BEATS="-e '{amd_df/event=0x41e,umask=0xce0,name=ccm0_0_cxl_read_beats/,amd_df/event=0x45e,umask=0xce0,name=ccm1_0_cxl_read_beats/,amd_df/event=0x49e,umask=0xce0,name=ccm2_0_cxl_read_beats/,amd_df/event=0x4de,umask=0xce0,name=ccm3_0_cxl_read_beats/,amd_df/event=0x51e,umask=0xce0,name=ccm4_0_cxl_read_beats/,amd_df/event=0x55e,umask=0xce0,name=ccm5_0_cxl_read_beats/,amd_df/event=0x59e,umask=0xce0,name=ccm6_0_cxl_read_beats/,amd_df/event=0x5de,umask=0xce0,name=ccm7_0_cxl_read_beats/,amd_df/event=0x41f,umask=0xce0,name=ccm0_1_cxl_read_beats/,amd_df/event=0x45f,umask=0xce0,name=ccm1_1_cxl_read_beats/,amd_df/event=0x49f,umask=0xce0,name=ccm2_1_cxl_read_beats/,amd_df/event=0x4df,umask=0xce0,name=ccm3_1_cxl_read_beats/,amd_df/event=0x51f,umask=0xce0,name=ccm4_1_cxl_read_beats/,amd_df/event=0x55f,umask=0xce0,name=ccm5_1_cxl_read_beats/,amd_df/event=0x59f,umask=0xce0,name=ccm6_1_cxl_read_beats/,amd_df/event=0x5df,umask=0xce0,name=ccm7_1_cxl_read_beats/}'"

#if there are any, otherwise just set to ""
if [ -n "$amd_umc_device_dirs" ]; then
# amd_umc_devices=$(echo $amd_umc_device_dirs | xargs -n 1 basename | awk -F'_' '{print $3}' | sort -V)
ZEN5ES_UMC=""
for n in $amd_umc_devices; do
ZEN5ES_UMC+="-e '{amd_umc_$n/event=0,name=umc_cyc_umc$n/,amd_umc_$n/event=0x14,rdwrmask=0,name=umc_data_cyc_umc$n/,amd_umc_$n/event=0x14,rdwrmask=2,name=umc_data_write_cyc_umc$n/}' "
done
  ZEN5ES_UMC=${ZEN5ES_UMC%,}
else
  ZEN5ES_UMC=""
fi


ZEN5ES_EVENTS="${DE_DISPATCH_STALL_CYCLE_DYNAMIC_TOKENS} ${FRONTEND_LATENCY} ${DE_SRC_OP_DISP_X86_DECODER_OP_CACHE} ${L2_PF_MISS_L2_HIT_L3} ${DE_DIS_OPS_FROM_DECODER_FP_INTEGER_DISPATCH} ${CS_CMP_CXL_WRITE_BEATS} ${CCM_CXL_READ_BEATS}"

PERF_PID=
wrapup() {
  kill "$PERF_PID"
}
trap wrapup SIGINT SIGTERM
perf_stat() {
  local ev="$1"
  local interval_ms="$2"
  perf stat -e $ev -x, -I "${interval_ms}" --per-socket -a --log-fd 1 &
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
  model_name=$(lscpu | grep -i "Model name" )
  if [[ $model_name =~ (AMD.*EPYC|100-000000976-14|100-000001458-01|100-000001460-02|100-000001537-04|100-000001463-04|100-000001535-05) ]]; then
    # zen5
    events="msr/aperf,name=cycles/ -e cpu/instructions,name=instructions/ -e msr/tsc,name=tsc/ -e msr/mperf,name=mperf/ ${COMMON_CORE_EVENTS} ${COMMON_DF_EVENTS} ${ZEN5_DMA} ${ZEN5_CSBW} ${ZEN5_EVENTS} ${ZEN5_UMC}"
  else
    # zen5es
    events="msr/aperf,name=cycles/ -e cpu/instructions,name=instructions/ -e msr/tsc,name=tsc/ -e msr/mperf,name=mperf/ ${COMMON_CORE_EVENTS} ${COMMON_DF_EVENTS} ${ZEN5_DMA} ${ZEN5_CSBW} ${ZEN5ES_EVENTS} ${ZEN5ES_UMC}"
  fi
  perf_stat "${events}" "${interval_ms}"
}
collect_counters "$1" 2>/tmp/${ME}.err
