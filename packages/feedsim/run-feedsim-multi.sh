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
# Load custom-built shared libraries from staging (for self-contained fbpkg).
# Must be set before any feedsim binary (LeafNodeRank, DriverNodeRank) runs.
if [ -d "${FEEDSIM_ROOT}/staging/lib" ]; then
    export LD_LIBRARY_PATH="${FEEDSIM_ROOT}/staging/lib:${FEEDSIM_ROOT}/staging/lib64:${LD_LIBRARY_PATH:-}"
fi
if [ -d "${FEEDSIM_ROOT}/third_party/libtorch/lib" ]; then
    export LD_LIBRARY_PATH="${FEEDSIM_ROOT}/third_party/libtorch/lib:${LD_LIBRARY_PATH:-}"
fi
FEEDSIM_LOG_PREFIX="${FEEDSIM_ROOT}/feedsim-multi-inst-${FIXQPS_SUFFIX}"
# Calculate number of instances based on physical cores (1 instance per 50 physical cores).
# nproc returns logical cores; divide by 2 when SMT is active to get physical cores.
NCPU="$(nproc)"
if [ -f /sys/devices/system/cpu/smt/active ] && [ "$(cat /sys/devices/system/cpu/smt/active)" -eq 1 ]; then
    PHYS_CORES="$(( NCPU / 2 ))"
else
    PHYS_CORES="$NCPU"
fi
NUM_INSTANCES="$(( ( PHYS_CORES + 49 ) / 50 ))"

NUM_ICACHE_ITERATIONS="1600000"

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

    -h Display this help and exit
    -n Number of parallel instances to run. Default: $(( ( PHYS_CORES + 49 ) / 50 ))
    -i Number of icache iterations to use. Default: 1600000

Any remaining arguments are passed to run.sh

EOF
}

SCRIPT_NAME="$(basename "$0")"
echo "${SCRIPT_NAME}: DCPERF_PERF_RECORD=${DCPERF_PERF_RECORD}"


while [ $# -ne 0 ]; do
    case $1 in
        -n)
            if [[ "$2" -gt 0 ]]; then
                NUM_INSTANCES="$2"
            fi
            ;;
        -i)
            NUM_ICACHE_ITERATIONS="$2"
            ;;

        -h|--help)
            show_help >&2
            exit 1
            ;;
        *)  # end of input
            break
    esac

    case $1 in
        -n|-i)
            if [ -z "$2" ]; then
                echo "Invalid option: '$1' requires an argument" 1>&2
                exit 1
            fi
            shift   # Additional shift for the argument
            ;;
    esac
    shift # pop the previously read argument
done



PORT=21212
PIDS=()

# Phase 5-A orchestration: start mock_services ONCE per host before
# spawning the LeafNodeRank instances. mock_services is stateless and
# serves all colocated leaves on a fixed port (21222 = LeafNodeRank's
# --mock_services_port default). Without this, LeafNodeRank's outbound
# fanout client cannot connect, the request handler falls back to
# legacy folly::futures::sleep, and the Phase 6 session-mode driver
# never produces QPS samples (search_qps.sh fails with "Could not
# find QPS in loadtest output" -> divide by zero).
MOCK_SERVICES_PORT=21222
# Binary lives under src/ — the install script unpacks the source tarball
# alongside run.sh and ninja builds into src/build/. run.sh's main loop
# already does `cd "${FEEDSIM_ROOT}/src"` before invoking LeafNodeRank;
# from this script we use the absolute src/ path directly.
MOCK_SERVICES_BIN="${FEEDSIM_ROOT}/src/build/workloads/ranking/mock_services/mock_services"
MOCK_SERVICES_LOG="${FEEDSIM_ROOT}/mock_services.log"
MOCK_SERVICES_PID=""

# Resolve --silesia-dir from forwarded args ($@); fall back to the default
# install layout (./silesia next to run.sh). mock_services requires Silesia
# bytes for its response generators.
function resolve_silesia_dir() {
    local args=("$@")
    local i=0
    while [ "$i" -lt "${#args[@]}" ]; do
        case "${args[$i]}" in
            --silesia-dir)
                echo "${args[$((i+1))]}"
                return
                ;;
            --silesia-dir=*)
                echo "${args[$i]#*=}"
                return
                ;;
        esac
        i=$((i+1))
    done
    echo "silesia"
}

SILESIA_DIR_ARG="$(resolve_silesia_dir "$@")"
if [[ "$SILESIA_DIR_ARG" != /* ]]; then
    SILESIA_DIR_ABS="${FEEDSIM_ROOT}/${SILESIA_DIR_ARG}"
else
    SILESIA_DIR_ABS="$SILESIA_DIR_ARG"
fi

function start_mock_services() {
    if [ ! -x "$MOCK_SERVICES_BIN" ]; then
        echo "ERROR: mock_services binary not found at $MOCK_SERVICES_BIN" >&2
        exit 1
    fi
    if [ ! -d "$SILESIA_DIR_ABS" ]; then
        echo "ERROR: Silesia directory not found at $SILESIA_DIR_ABS (mock_services requires it)" >&2
        exit 1
    fi
    # Size the IO worker pool to one thread per logical core so the fbthrift
    # server can absorb the simultaneous startup-probe burst from every
    # LeafNodeRank thread (88-176 probes on BGM/Turin) without saturating
    # its default 6-pool sizing and timing out per-client probes.
    local mock_io_threads
    mock_io_threads="$(nproc)"

    # Latency-shaping knobs (env-overridable). Defaults match the
    # mock_services compiled-in defaults: 200ms cap, 0us offset, 100us
    # skip threshold. Tune via MOCK_LATENCY_CAP_US / MOCK_LATENCY_OFFSET_US
    # / MOCK_LATENCY_SKIP_THRESHOLD_US to shape the per-RPC simulated
    # delay -- rpc_dist.json contains very long-tail values (p99 ~8s,
    # max ~28s) that make each fanout block on the slowest call.
    local mock_latency_cap_us="${MOCK_LATENCY_CAP_US:-200000}"
    local mock_latency_offset_us="${MOCK_LATENCY_OFFSET_US:-0}"
    local mock_latency_skip_threshold_us="${MOCK_LATENCY_SKIP_THRESHOLD_US:-100}"

    echo "Starting mock_services on port ${MOCK_SERVICES_PORT} (silesia=${SILESIA_DIR_ABS}, io_threads=${mock_io_threads}, cap_us=${mock_latency_cap_us}, offset_us=${mock_latency_offset_us}, skip_threshold_us=${mock_latency_skip_threshold_us})"
    "$MOCK_SERVICES_BIN" \
        --port="$MOCK_SERVICES_PORT" \
        --mock_io_threads="$mock_io_threads" \
        --latency_cap_us="$mock_latency_cap_us" \
        --latency_offset_us="$mock_latency_offset_us" \
        --latency_skip_threshold_us="$mock_latency_skip_threshold_us" \
        --silesia_dir="$SILESIA_DIR_ABS" \
        > "$MOCK_SERVICES_LOG" 2>&1 &
    MOCK_SERVICES_PID=$!

    # TCP-poll readiness (mirrors the LeafNodeRank wait loop in run.sh).
    local max_attempts=30
    local attempt=0
    while [ "$attempt" -lt "$max_attempts" ]; do
        if (echo > /dev/tcp/localhost/"$MOCK_SERVICES_PORT") 2>/dev/null; then
            echo "mock_services is ready (port $MOCK_SERVICES_PORT accepting connections, pid=$MOCK_SERVICES_PID)"
            return 0
        fi
        # Bail early if the process died.
        if ! kill -0 "$MOCK_SERVICES_PID" 2>/dev/null; then
            echo "ERROR: mock_services died during startup. Tail of log:" >&2
            tail -40 "$MOCK_SERVICES_LOG" >&2
            exit 1
        fi
        attempt=$((attempt + 1))
        sleep 1
    done
    echo "ERROR: mock_services failed to become ready within ${max_attempts}s" >&2
    tail -40 "$MOCK_SERVICES_LOG" >&2
    kill -SIGTERM "$MOCK_SERVICES_PID" 2>/dev/null || true
    exit 1
}

function stop_mock_services() {
    if [ -n "$MOCK_SERVICES_PID" ] && kill -0 "$MOCK_SERVICES_PID" 2>/dev/null; then
        echo "Stopping mock_services (pid=$MOCK_SERVICES_PID)"
        kill -SIGINT "$MOCK_SERVICES_PID" 2>/dev/null || true
        # Give it a moment to exit cleanly, then force-kill.
        for _ in 1 2 3 4 5; do
            kill -0 "$MOCK_SERVICES_PID" 2>/dev/null || break
            sleep 1
        done
        kill -SIGKILL "$MOCK_SERVICES_PID" 2>/dev/null || true
    fi
}

# When LEAFNODE_USE_LEGACY_SLEEP=1, run.sh forwards --use_legacy_sleep to
# LeafNodeRank and the leaf takes the legacy folly::futures::sleep path
# instead of fanning RPCs to mock_services. Skip the mock_services side
# process entirely in that case so later diffs in the stack can run
# end-to-end integration tests without the mock_services dependency.
if [ "${LEAFNODE_USE_LEGACY_SLEEP:-0}" = "1" ]; then
    echo "Skipping mock_services startup (LEAFNODE_USE_LEGACY_SLEEP=1)"
else
    trap stop_mock_services EXIT INT TERM
    start_mock_services
fi

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
    CMD="IS_AUTOSCALE_RUN=${NUM_INSTANCES} taskset --cpu-list ${CORE_RANGE} ${FEEDSIM_ROOT}/run.sh -p ${PORT} -i ${NUM_ICACHE_ITERATIONS} -o feedsim_results_${FIXQPS_SUFFIX}${i}.txt  $*"
    echo "$CMD" > "${FEEDSIM_LOG_PREFIX}${i}.log"
    # shellcheck disable=SC2068,SC2069
    IS_AUTOSCALE_RUN=${NUM_INSTANCES} stdbuf -i0 -o0 -e0 taskset --cpu-list "${CORE_RANGE}" "${FEEDSIM_ROOT}"/run.sh -p "${PORT}" -i "${NUM_ICACHE_ITERATIONS}" -o "feedsim_results_${FIXQPS_SUFFIX}${i}.txt" $@ 2>&1 > "${FEEDSIM_LOG_PREFIX}${i}.log" &
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
