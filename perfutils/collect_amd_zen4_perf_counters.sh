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

L3_ACCESSES_EV='amd_l3/event=0x04,umask=0xFF,coreid=0x0,enallslices=0x1,enallcores=0x1,sliceid=0x0,threadmask=0x3,name=l3_acceses/'
L3_MISSES_EV='amd_l3/event=0x04,umask=0x01,coreid=0x0,enallslices=0x1,enallcores=0x1,sliceid=0x0,threadmask=0x3,name=l3_misses/'

L3_CACHE_GROUP="${L3_ACCESSES_EV},${L3_MISSES_EV}"

LOCAL_UMC_A_READ_REQS_EV='amd_df/event=0x01F,umask=0x7FE,name=umc_a_read_requests/'
LOCAL_UMC_B_READ_REQS_EV='amd_df/event=0x05F,umask=0x7FE,name=umc_b_read_requests/'
LOCAL_UMC_C_READ_REQS_EV='amd_df/event=0x09F,umask=0x7FE,name=umc_c_read_requests/'
LOCAL_UMC_D_READ_REQS_EV='amd_df/event=0x0DF,umask=0x7FE,name=umc_d_read_requests/'

LOCAL_UMC_E_READ_REQS_EV='amd_df/event=0x11F,umask=0x7FE,name=umc_e_read_requests/'
LOCAL_UMC_F_READ_REQS_EV='amd_df/event=0x15F,umask=0x7FE,name=umc_f_read_requests/'
LOCAL_UMC_G_READ_REQS_EV='amd_df/event=0x19F,umask=0x7FE,name=umc_g_read_requests/'
LOCAL_UMC_H_READ_REQS_EV='amd_df/event=0x1DF,umask=0x7FE,name=umc_h_read_requests/'

LOCAL_UMC_I_READ_REQS_EV='amd_df/event=0x21F,umask=0x7FE,name=umc_i_read_requests/'
LOCAL_UMC_J_READ_REQS_EV='amd_df/event=0x25F,umask=0x7FE,name=umc_j_read_requests/'
LOCAL_UMC_K_READ_REQS_EV='amd_df/event=0x29F,umask=0x7FE,name=umc_k_read_requests/'
LOCAL_UMC_L_READ_REQS_EV='amd_df/event=0x2DF,umask=0x7FE,name=umc_l_read_requests/'


LOCAL_UMC_A_WRITE_REQS_EV='amd_df/event=0x01F,umask=0x7FF,name=umc_a_write_requests/'
LOCAL_UMC_B_WRITE_REQS_EV='amd_df/event=0x05F,umask=0x7FF,name=umc_b_write_requests/'
LOCAL_UMC_C_WRITE_REQS_EV='amd_df/event=0x09F,umask=0x7FF,name=umc_c_write_requests/'
LOCAL_UMC_D_WRITE_REQS_EV='amd_df/event=0x0DF,umask=0x7FF,name=umc_d_write_requests/'

LOCAL_UMC_E_WRITE_REQS_EV='amd_df/event=0x11F,umask=0x7FF,name=umc_e_write_requests/'
LOCAL_UMC_F_WRITE_REQS_EV='amd_df/event=0x15F,umask=0x7FF,name=umc_f_write_requests/'
LOCAL_UMC_G_WRITE_REQS_EV='amd_df/event=0x19F,umask=0x7FF,name=umc_g_write_requests/'
LOCAL_UMC_H_WRITE_REQS_EV='amd_df/event=0x1DF,umask=0x7FF,name=umc_h_write_requests/'


LOCAL_UMC_I_WRITE_REQS_EV='amd_df/event=0x21F,umask=0x7FF,name=umc_i_write_requests/'
LOCAL_UMC_J_WRITE_REQS_EV='amd_df/event=0x25F,umask=0x7FF,name=umc_j_write_requests/'
LOCAL_UMC_K_WRITE_REQS_EV='amd_df/event=0x29F,umask=0x7FF,name=umc_k_write_requests/'
LOCAL_UMC_L_WRITE_REQS_EV='amd_df/event=0x2DF,umask=0x7FF,name=umc_l_write_requests/'



SOCKET_WRITE_BW_GROUP="{${LOCAL_UMC_A_WRITE_REQS_EV},${LOCAL_UMC_B_WRITE_REQS_EV},${LOCAL_UMC_C_WRITE_REQS_EV},${LOCAL_UMC_D_WRITE_REQS_EV},${LOCAL_UMC_E_WRITE_REQS_EV},${LOCAL_UMC_F_WRITE_REQS_EV},${LOCAL_UMC_G_WRITE_REQS_EV},${LOCAL_UMC_H_WRITE_REQS_EV},${LOCAL_UMC_I_WRITE_REQS_EV},${LOCAL_UMC_J_WRITE_REQS_EV},${LOCAL_UMC_K_WRITE_REQS_EV},${LOCAL_UMC_L_WRITE_REQS_EV}}"


SOCKET_READ_BW_GROUP="{${LOCAL_UMC_A_READ_REQS_EV},${LOCAL_UMC_B_READ_REQS_EV},${LOCAL_UMC_C_READ_REQS_EV},${LOCAL_UMC_D_READ_REQS_EV},${LOCAL_UMC_E_READ_REQS_EV},${LOCAL_UMC_F_READ_REQS_EV},${LOCAL_UMC_G_READ_REQS_EV},${LOCAL_UMC_H_READ_REQS_EV},${LOCAL_UMC_I_READ_REQS_EV},${LOCAL_UMC_J_READ_REQS_EV},${LOCAL_UMC_K_READ_REQS_EV},${LOCAL_UMC_L_READ_REQS_EV}}"

PERF_PID=
wrapup() {
  kill "$PERF_PID"
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
  events="{cycles,instructions},${L3_CACHE_GROUP},${SOCKET_READ_BW_GROUP},${SOCKET_WRITE_BW_GROUP}"
  perf_stat "${events}" "${interval_ms}"
}

collect_counters "$1" 2>/tmp/${ME}.err
