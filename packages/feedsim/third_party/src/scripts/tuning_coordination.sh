#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
# Search variables are supplied by the sourcing search_qps.sh script.
# shellcheck disable=SC2154

coordination_init() {
    COORD_ENABLED=0
    COORD_ROUND=0
    COORD_DRIVER_PID=""
    COORD_COMPLETE=0
    if [ -z "${FEEDSIM_COORDINATION_DIR:-}" ] || [ "${IS_AUTOSCALE_RUN:-1}" -le 1 ]; then
        return
    fi
    COORD_TIMEOUT="${FEEDSIM_COORDINATION_TIMEOUT_SECS:-3600}"
    if ! [[ "$COORD_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || [ "$COORD_TIMEOUT" -gt 2147483647 ]; then
        echo "ERROR: invalid FEEDSIM_COORDINATION_TIMEOUT_SECS: $COORD_TIMEOUT" >&2
        exit 1
    fi
    if ! [[ "${FEEDSIM_INSTANCE_ID:-}" =~ ^[1-9][0-9]*$ ]] ||
        [ "$FEEDSIM_INSTANCE_ID" -gt "$IS_AUTOSCALE_RUN" ] ||
        [ ! -d "$FEEDSIM_COORDINATION_DIR" ]; then
        echo "ERROR: invalid feedsim coordination directory or instance ID" >&2
        exit 1
    fi
    COORD_ENABLED=1
    printf '%s\n' "$$" > "$FEEDSIM_COORDINATION_DIR/worker.$FEEDSIM_INSTANCE_ID"
    trap 'coordination_cleanup $?' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
}

coordination_stop_driver() {
    [ -n "$COORD_DRIVER_PID" ] || return 0
    kill -INT "$COORD_DRIVER_PID" 2>/dev/null || true
    local deadline=$((SECONDS + 5))
    while kill -0 "$COORD_DRIVER_PID" 2>/dev/null && [ "$SECONDS" -lt "$deadline" ]; do
        sleep 0.1
    done
    kill -KILL "$COORD_DRIVER_PID" 2>/dev/null || true
    wait "$COORD_DRIVER_PID" 2>/dev/null || true
    COORD_DRIVER_PID=""
}

coordination_cleanup() {
    if [ "$COORD_COMPLETE" -ne 1 ] || [ "$1" -ne 0 ]; then
        touch "$FEEDSIM_COORDINATION_DIR/failed.$FEEDSIM_INSTANCE_ID"
    fi
    coordination_stop_driver
}

# A tuned peer participates in subsequent probe barriers through its hold load.
coordination_barrier() {
    [ "$COORD_ENABLED" -eq 1 ] || return 0
    local phase="$1" allow_hold="${2:-0}" peer ready pid
    local deadline=$((SECONDS + COORD_TIMEOUT))
    touch "$FEEDSIM_COORDINATION_DIR/$phase.$FEEDSIM_INSTANCE_ID"
    while :; do
        ready=1
        if [ -n "$COORD_DRIVER_PID" ] && ! kill -0 "$COORD_DRIVER_PID" 2>/dev/null; then
            echo "ERROR: coordination driver exited at $phase" >&2
            exit 1
        fi
        for ((peer=1; peer<=IS_AUTOSCALE_RUN; peer++)); do
            if [ -f "$FEEDSIM_COORDINATION_DIR/failed.$peer" ]; then
                echo "ERROR: feedsim instance $peer failed at $phase" >&2
                exit 1
            fi
            if [ -f "$FEEDSIM_COORDINATION_DIR/$phase.$peer" ]; then
                continue
            fi
            if [ "$allow_hold" -eq 1 ] && [ -f "$FEEDSIM_COORDINATION_DIR/tuned.$peer" ]; then
                continue
            fi
            if [ -f "$FEEDSIM_COORDINATION_DIR/worker.$peer" ]; then
                read -r pid < "$FEEDSIM_COORDINATION_DIR/worker.$peer"
                if ! kill -0 "$pid" 2>/dev/null; then
                    echo "ERROR: feedsim instance $peer exited at $phase" >&2
                    exit 1
                fi
            fi
            ready=0
        done
        if [ "$ready" -eq 1 ]; then
            printf 'coordination: instance=%s phase=%s ready\n' "$FEEDSIM_INSTANCE_ID" "$phase"
            return
        fi
        if [ "$SECONDS" -ge "$deadline" ]; then
            echo "ERROR: feedsim coordination timed out at $phase after ${COORD_TIMEOUT}s" >&2
            exit 1
        fi
        sleep 0.1
    done
}

coordination_probe_start() {
    [ "$COORD_ENABLED" -eq 1 ] || return 0
    COORD_ROUND=$((COORD_ROUND + 1))
    COORD_DRIVER_PID="$LOADTEST_PID"
    coordination_barrier "probe_start.$COORD_ROUND" 1
}

coordination_probe_end() {
    [ "$COORD_ENABLED" -eq 1 ] || return 0
    coordination_barrier "probe_end.$COORD_ROUND" 1
}

coordination_finish_tuning() {
    [ "$COORD_ENABLED" -eq 1 ] || return 0
    local threads_arg
    threads_arg=$(driver_thread_args "$cur_qps")
    local hold_log="${output_csv_file}.coordination-hold.log"
    # shellcheck disable=SC2086
    $command $threads_arg "--qps=$cur_qps" $adaptive_depth_arg > "$hold_log" 2>&1 &
    COORD_DRIVER_PID=$!
    printf 'coordination: instance=%s hold_start pid=%s qps=%s depth=%s log=%s\n' \
        "$FEEDSIM_INSTANCE_ID" "$COORD_DRIVER_PID" "$cur_qps" "$adaptive_depth_arg" "$hold_log"
    sleep 7
    coordination_barrier tuned
    coordination_stop_driver
    coordination_barrier final_ready
}
