#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

DEFAULT_INTERVAL_SECS=5

# Olympus is not Neoverse V2. Collect only architected Arm core events whose
# meanings are implementation-independent and have been validated on Vera.
# Keep every ratio's operands in one hardware group.
PERF_EVENTS=(
  -e "{cpu_cycles,instructions,r04,r03}"
  -e "{r14,r01}"
  -e "{r21,r22}"
)
PERF_PID=
rc=
cleanup() {
  rc=$?
  set +e
  if [[ -n "$PERF_PID" ]]; then
    kill -INT "$PERF_PID"
    wait "$PERF_PID"
  fi
  exit "$rc"
}
trap cleanup EXIT
for sig in 1 2 3 13 15; do
  eval "trap 'exit $((sig + 128))' $sig"
done

add_device_group() {
  local pmu_prefix="$1"
  shift
  local device_dir device event
  local -a device_events
  for device_dir in /sys/bus/event_source/devices/"${pmu_prefix}"*; do
    [[ -d "$device_dir" ]] || continue
    device="${device_dir##*/}"
    device_events=()
    for event in "$@"; do
      if [[ -f "$device_dir/events/$event" ]]; then
        device_events+=("$device/$event/")
      fi
    done
    if ((${#device_events[@]} > 0)); then
      local IFS=,
      PERF_EVENTS+=(-e "{${device_events[*]}}")
    fi
  done
}

# Keep each ratio's operands in one hardware group so perf cannot schedule
# numerator and denominator in different multiplexing slices.
add_device_group \
  nvidia_ucf_pmu \
  mem_bytes_rd \
  mem_bytes_wr \
  slc_access_rd \
  slc_hit_rd \
  slc_refill_rd

add_device_group nvidia_cmem_latency_pmu cycles rd_req rd_cum_outs

interval="${1:-$DEFAULT_INTERVAL_SECS}"
if ! [[ "$interval" =~ ^[1-9][0-9]*$ ]]; then
  echo "interval must be a positive integer" >&2
  exit 2
fi

perf stat \
  "${PERF_EVENTS[@]}" \
  -x, \
  -I "$((interval * 1000))" \
  -a \
  --log-fd 1 &
PERF_PID=$!
wait "$PERF_PID"
PERF_PID=
