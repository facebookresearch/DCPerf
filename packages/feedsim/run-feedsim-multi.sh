#!/bin/bash

FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
FEEDSIM_LOG_PREFIX="${FEEDSIM_ROOT}/feedsim-multi-inst-"
NCPU="$(nproc)"
NUM_INSTANCES="$(( ( NCPU + 99 ) / 100 ))"

if [[ -n "$1" ]] && [[ "$1" -gt 0 ]]; then
    NUM_INSTANCES="$1"
    shift
fi

PORT=11211
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

    PHY_CORE_BASE="$((CORES_PER_INST * inst_id))"
    PHY_CORE_END="$((PHY_CORE_BASE + CORES_PER_INST - 1))"

    RES="${PHY_CORE_BASE}-${PHY_CORE_END}"
    if [ "$has_smt" -eq 1 ]; then
        SMT_BASE="$((NPROC / 2 + CORES_PER_INST * inst_id))"
        SMT_END="$((SMT_BASE + CORES_PER_INST - 1))"
        RES="${RES},${SMT_BASE}-${SMT_END}"
    fi

    echo "$RES"
}

# shellcheck disable=SC2086
for i in $(seq 1 ${NUM_INSTANCES}); do
    CORE_RANGE="$(get_cpu_range "${NUM_INSTANCES}" "$((i - 1))")"
    CMD="taskset --cpu-list ${CORE_RANGE} ${FEEDSIM_ROOT}/run.sh -p ${PORT} -o feedsim_results_${i}.txt $*"
    echo "$CMD" > "${FEEDSIM_LOG_PREFIX}${i}.log"
    # shellcheck disable=SC2068,SC2069
    stdbuf -i0 -o0 -e0 taskset --cpu-list "${CORE_RANGE}" "${FEEDSIM_ROOT}"/run.sh -p "${PORT}" -o "feedsim_results_${i}.txt" $@ 2>&1 > "${FEEDSIM_LOG_PREFIX}${i}.log" &
    PIDS+=("$!")
    PHY_CORE_ID=$((PHY_CORE_ID + CORES_PER_INST))
    SMT_ID=$((SMT_ID + CORES_PER_INST))
    PORT=$((PORT + 1))
done

# shellcheck disable=SC2068,SC2069
for pid in ${PIDS[@]}; do
    wait "$pid" 2>&1 >/dev/null
done

function analyze_and_print_results() {
    echo "{"
    total_req_qps=0.0
    total_actual_qps=0.0
    avg_latency=0.0
    successful_insts=0
    target_percentile=""
    target_latency=0.0
    # shellcheck disable=SC2086
    for i in $(seq 1 ${NUM_INSTANCES}); do
        final_requested_qps="$(grep -oP 'final requested_qps = \K[0-9.]+' "${FEEDSIM_LOG_PREFIX}${i}.log")"
        if [ -z "$final_requested_qps" ]; then
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
    done

    avg_latency="$(echo "scale=2; 1.0 * ${avg_latency} / ${successful_insts}" | bc)"
    echo "    \"overall\": {\"final_requested_qps\": ${total_req_qps}, \"final_achieved_qps\": ${total_actual_qps}, \"average_latency_msec\": ${avg_latency}},"
    echo "    \"target_percentile\": \"${target_percentile}\","
    echo "    \"target_latency_msec\": \"${target_latency}\","
    echo "    \"spawned_instances\": \"${NUM_INSTANCES}\","
    echo "    \"successful_instances\": ${successful_insts}"
    echo "}"
}

# shellcheck disable=SC2069
if /usr/bin/env jq -h 2>&1 >/dev/null; then
    analyze_and_print_results | jq
else
    analyze_and_print_results
fi
