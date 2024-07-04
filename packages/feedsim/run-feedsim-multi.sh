#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BREPS_LFILE=/tmp/feedsim_log.txt
IS_FIXED_QPS=0
FIXQPS_SUFFIX=""
THIS_CMD="$0 $*"

if [[ "$THIS_CMD" =~ -q.*[0-9]+ ]]; then
    IS_FIXED_QPS=1
    FIXQPS_SUFFIX="fixqps-"
fi

FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
FEEDSIM_LOG_PREFIX="${FEEDSIM_ROOT}/feedsim-multi-inst-${FIXQPS_SUFFIX}"
NCPU="$(nproc)"
NUM_INSTANCES="$(( ( NCPU + 99 ) / 100 ))"

if [[ -n "$1" ]] && [[ "$1" -gt 0 ]]; then
    NUM_INSTANCES="$1"
    shift
fi

PORT=21212
PIDS=()

function get_cpu_range() {
    total_instances="$1"
    inst_id="$2"
    has_smt="$(cat /sys/devices/system/cpu/smt/active)"

    NPROC="$(nproc)"
    if [ "$has_smt" -eq 1 ]; then
        NCORES="$((NPROC / 2))"
    else
        NCORES="$NPROC"
    fi
    CORES_PER_INST="$((NCORES / total_instances))"
    REMAINING_CORES="$((NCORES - CORES_PER_INST * total_instances))"
    EXTRA_CORE=0
    OFFSET=0
    if [ "$inst_id" -lt "$REMAINING_CORES" ]; then
        EXTRA_CORE=1
        OFFSET="$inst_id"
    else
        EXTRA_CORE=0
        OFFSET="$REMAINING_CORES"
    fi

    PHY_CORE_BASE="$((CORES_PER_INST * inst_id + OFFSET))"
    PHY_CORE_END="$((PHY_CORE_BASE + CORES_PER_INST + EXTRA_CORE - 1))"

    RES="${PHY_CORE_BASE}-${PHY_CORE_END}"
    if [ "$has_smt" -eq 1 ]; then
        SMT_BASE="$((NPROC / 2 + CORES_PER_INST * inst_id + OFFSET))"
        SMT_END="$((SMT_BASE + CORES_PER_INST + EXTRA_CORE - 1))"
        RES="${RES},${SMT_BASE}-${SMT_END}"
    fi

    echo "$RES"
}

echo > $BREPS_LFILE
# shellcheck disable=SC2086
for i in $(seq 1 ${NUM_INSTANCES}); do
    CORE_RANGE="$(get_cpu_range "${NUM_INSTANCES}" "$((i - 1))")"
    CMD="IS_AUTOSCALE_RUN=1 taskset --cpu-list ${CORE_RANGE} ${FEEDSIM_ROOT}/run.sh -p ${PORT} -o feedsim_results_${FIXQPS_SUFFIX}${i}.txt $*"
    echo "$CMD" > "${FEEDSIM_LOG_PREFIX}${i}.log"
    # shellcheck disable=SC2068,SC2069
    IS_AUTOSCALE_RUN=1 stdbuf -i0 -o0 -e0 taskset --cpu-list "${CORE_RANGE}" "${FEEDSIM_ROOT}"/run.sh -p "${PORT}" -o "feedsim_results_${FIXQPS_SUFFIX}${i}.txt" $@ 2>&1 > "${FEEDSIM_LOG_PREFIX}${i}.log" &
    PIDS+=("$!")
    PHY_CORE_ID=$((PHY_CORE_ID + CORES_PER_INST))
    SMT_ID=$((SMT_ID + CORES_PER_INST))
    PORT=$((PORT + 1))
done

# shellcheck disable=SC2068,SC2069
for pid in ${PIDS[@]}; do
    wait "$pid" 2>&1 >/dev/null
done

BC_MAX_FN='define max (a, b) { if (a >= b) return (a); return (b); }'
BC_MIN_FN='define min (a, b) { if (a <= b) return (a); return (b); }'
function analyze_and_print_results() {
    echo "{"
    total_req_qps=0.0
    total_actual_qps=0.0
    avg_latency=0.0
    successful_insts=0
    target_percentile=""
    target_latency=0.0
    min_qps=99999.9
    max_qps=0.0
    max_req_qps=0.0

    # shellcheck disable=SC2086
    for i in $(seq 1 ${NUM_INSTANCES}); do
        final_requested_qps="$(grep -oP 'final requested_qps = \K[0-9.]+' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        if [ -z "$final_requested_qps" ]; then
            min_qps=0.0
            continue
        fi
        successful_insts="$((successful_insts + 1))"
        measured_qps="$(grep -oP 'final.*measured_qps = \K[0-9.]+' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        latency="$(grep -oP 'final.*latency = \K[0-9.]+' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        target_percentile="$(grep -oP 'Searching for QPS where \K[0-9p]+' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        target_latency="$(grep -oP 'Searching for.*latency <= \K[0-9]+(?= msec)' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        echo "    \"${i}\": {\"final_requested_qps\": ${final_requested_qps}, \"final_achieved_qps\": ${measured_qps}, \"final_latency_msec\": ${latency}},"
        total_req_qps="$(echo "${total_req_qps} + ${final_requested_qps}" | bc)"
        total_actual_qps="$(echo "${total_actual_qps} + ${measured_qps}" | bc)"
        avg_latency="$(echo "${avg_latency} + ${latency}" | bc)"
        min_qps="$(echo "${BC_MIN_FN}; min(${min_qps}, ${measured_qps})" | bc)"
        max_qps="$(echo "${BC_MAX_FN}; max(${max_qps}, ${measured_qps})" | bc)"
        max_req_qps="$(echo "${BC_MAX_FN}; max(${max_req_qps}, ${final_requested_qps})" | bc)"
    done

    avg_latency="$(echo "scale=2; 1.0 * ${avg_latency} / ${successful_insts}" | bc)"
    echo "    \"overall\": {\"final_requested_qps\": ${total_req_qps}, \"final_achieved_qps\": ${total_actual_qps}, \"average_latency_msec\": ${avg_latency}},"
    echo "    \"target_percentile\": \"${target_percentile}\","
    echo "    \"target_latency_msec\": \"${target_latency}\","
    echo "    \"spawned_instances\": \"${NUM_INSTANCES}\","
    echo "    \"successful_instances\": ${successful_insts},"
    echo "    \"min_qps\": ${min_qps},"
    echo "    \"max_qps\": ${max_qps},"
    echo "    \"is_fixed_qps\": ${IS_FIXED_QPS}"
    echo "}"
    if [[ "$(echo "${min_qps} < 0.8 * ${max_qps}" | bc)" = "1" ]]; then
        # ceil(max_req_qps)
        echo "(${max_req_qps} + 1) / 1" | bc  > /tmp/max_req_qps
        return 1
    else
        return 0
    fi
}

is_unstable_run=0
# shellcheck disable=SC2069
if /usr/bin/env jq -h 2>&1 >/dev/null; then
    analyze_and_print_results | jq
    is_unstable_run="${PIPESTATUS[0]}"
else
    analyze_and_print_results
    is_unstable_run="$?"
fi

# rerun this program with fixed qps if detecting high variance
if [[ "$is_unstable_run" = 1 ]] && [[ "$IS_FIXED_QPS" = 0 ]] && [[ -z "$IS_RERUN" ]]; then
    max_req_qps="$(cat /tmp/max_req_qps)"
    echo "Detected unstable run - rerunning with fixed QPS at ${max_req_qps}..."
    # shellcheck disable=SC2068
    sleep 60
    IS_RERUN=1 $THIS_CMD -q "${max_req_qps}"
fi
