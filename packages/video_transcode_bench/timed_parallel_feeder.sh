#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Timed parallel feeder for VideoTranscodeBench.
# Feeds jobs from a command file to GNU parallel, stopping when the time
# limit is reached. In-flight jobs are allowed to finish gracefully.
# A joblog is written so callers can determine which jobs succeeded.
#
# Usage: timed_parallel_feeder.sh <jobs_file> <num_jobs> <max_time_secs> <joblog_file>
#   max_time_secs=0 means no time limit (original behavior).

set -Eeo pipefail

JOBS_FILE="$1"
NUM_JOBS="$2"
MAX_TIME="$3"
JOBLOG_FILE="$4"

if [ -z "$JOBS_FILE" ] || [ -z "$NUM_JOBS" ] || [ -z "$MAX_TIME" ] || [ -z "$JOBLOG_FILE" ]; then
    echo "Usage: $0 <jobs_file> <num_jobs> <max_time_secs> <joblog_file>" >&2
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
    # Run parallel in the background with all jobs available.
    # After the time limit, send SIGTERM to parallel which makes it stop
    # spawning new jobs while letting in-flight jobs finish gracefully.
    parallel --joblog "$JOBLOG_FILE" -j "$NUM_JOBS" < "$JOBS_FILE" &
    PARALLEL_PID=$!

    # Background timer: sends SIGTERM after MAX_TIME seconds
    ( sleep "$MAX_TIME" && kill -TERM "$PARALLEL_PID" 2>/dev/null ) &
    TIMER_PID=$!

    # Wait for parallel to finish (either naturally or after SIGTERM)
    wait "$PARALLEL_PID" 2>/dev/null || true

    # Clean up the timer if parallel finished before the time limit
    kill "$TIMER_PID" 2>/dev/null || true
    wait "$TIMER_PID" 2>/dev/null || true
fi
