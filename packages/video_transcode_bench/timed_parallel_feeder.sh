#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Timed parallel feeder for VideoTranscodeBench.
# Feeds a bounded number of jobs from a command file to GNU parallel.
# When a max_time is set, the number of jobs is chosen so that total
# wall-clock time approximates max_time on the target machine.
# A joblog is written so callers can determine which jobs succeeded.
#
# Usage: timed_parallel_feeder.sh <jobs_file> <num_jobs> <max_time_secs> <joblog_file> [durations_file]
#   max_time_secs=0 means no time limit (original behavior).
#   durations_file (optional): per-job durations from a prior prep run,
#     one value per line in the same order as jobs_file. Used to compute
#     how many jobs fit within max_time on this machine's core count.

set -Eeo pipefail

JOBS_FILE="$1"
NUM_JOBS="$2"
MAX_TIME="$3"
JOBLOG_FILE="$4"
DURATIONS_FILE="${5:-}"

if [ -z "$JOBS_FILE" ] || [ -z "$NUM_JOBS" ] || [ -z "$MAX_TIME" ] || [ -z "$JOBLOG_FILE" ]; then
    echo "Usage: $0 <jobs_file> <num_jobs> <max_time_secs> <joblog_file> [durations_file]" >&2
    exit 1
fi

if [ ! -f "$JOBS_FILE" ]; then
    echo "Error: jobs file '$JOBS_FILE' not found" >&2
    exit 1
fi

if [ "$MAX_TIME" -le 0 ]; then
    # No time limit: run all jobs (original behavior)
    parallel --joblog "$JOBLOG_FILE" -j "$NUM_JOBS" < "$JOBS_FILE"
else
    TOTAL_JOBS=$(wc -l < "$JOBS_FILE")
    TARGET_CORE_SECS=$(( MAX_TIME * NUM_JOBS ))

    if [ -n "$DURATIONS_FILE" ] && [ -f "$DURATIONS_FILE" ]; then
        # Compute MAX_JOBS from actual per-job durations: accumulate
        # durations until sum >= max_time * num_cores (target core-seconds).
        MAX_JOBS=$(awk -v target="$TARGET_CORE_SECS" '
            { sum += $1; if (sum >= target) { print NR; exit } }
            END { if (sum < target) print NR }
        ' "$DURATIONS_FILE")
    else
        # Fallback: estimate 2x jobs as a buffer (no duration data)
        MAX_JOBS=$(( MAX_TIME * NUM_JOBS * 2 ))
    fi

    if [ "$MAX_JOBS" -lt "$TOTAL_JOBS" ]; then
        head -n "$MAX_JOBS" "$JOBS_FILE" | parallel --joblog "$JOBLOG_FILE" -j "$NUM_JOBS" || true
    else
        parallel --joblog "$JOBLOG_FILE" -j "$NUM_JOBS" < "$JOBS_FILE" || true
    fi
fi
