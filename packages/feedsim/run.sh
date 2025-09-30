#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeo pipefail
trap cleanup SIGINT SIGTERM ERR EXIT

BREPS_LFILE=/tmp/feedsim_log.txt

SCRIPT_NAME="$(basename "$0")"
echo "${SCRIPT_NAME}: DCPERF_PERF_RECORD=${DCPERF_PERF_RECORD}"

function benchreps_tell_state () {
    date +"%Y-%m-%d_%T ${1}" >> $BREPS_LFILE
}


# Assumes run.sh is copied to the benchmark directory
#  ${BENCHPRESS_ROOT}/feedsim/run.sh

# Function for BC
BC_MAX_FN='define max (a, b) { if (a >= b) return (a); return (b); }'
BC_MIN_FN='define min (a, b) { if (a <= b) return (a); return (b); }'

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
FEEDSIM_ROOT_SRC="${FEEDSIM_ROOT}/src"

# Thrift threads: scale with logical CPUs till 216. Having more than that
# will risk running out of memory and getting killed
IS_SMT_ON="$(cat /sys/devices/system/cpu/smt/active)"
THRIFT_THREADS_DEFAULT="$(echo "${BC_MIN_FN}; min($(nproc), 216)" | bc)"
EVENTBASE_THREADS_DEFAULT=4  # 4 should suffice. Tune up if threads are saturated.
SRV_THREADS_DEFAULT=8        # 8 should also suffice for most purposes
if [[ "$IS_SMT_ON" = 1 ]]; then
  RANKING_THREADS_DEFAULT="$(( $(nproc) * 7/20))"  # 7/20 is 0.35 cpu factor
  SRV_IO_THREADS_DEFAULT="$(echo "${BC_MIN_FN}; min($(nproc) * 7/20, 55)" | bc)" # 0.35 cpu factor, max 55
  DRIVER_THREADS="$(echo "scale=2; $(nproc) / 5.0 + 0.5 " | bc )"  # Driver threads, rounds nearest.
  DRIVER_THREADS="${DRIVER_THREADS%.*}"  # Truncate decimal fraction.
  DRIVER_THREADS="$(echo "${BC_MAX_FN}; max(${DRIVER_THREADS:-0}, 4)" | bc )" # At least 4 threads.
else
  RANKING_THREADS_DEFAULT="$(( $(nproc) * 15/20))"  # 15/20 is 0.75 cpu factor
  SRV_IO_THREADS_DEFAULT="$(echo "${BC_MIN_FN}; min($(nproc) * 11/20, 55)" | bc)" # 0.55 cpu factor, max 55
  DRIVER_THREADS="$(echo "scale=2; $(nproc) / 4.0 + 0.5 " | bc )"  # Driver threads, rounds nearest.
  DRIVER_THREADS="${DRIVER_THREADS%.*}"  # Truncate decimal fraction.
  DRIVER_THREADS="$(echo "${BC_MAX_FN}; max(${DRIVER_THREADS:-0}, 4)" | bc )" # At least 4 threads.
fi

show_help() {
cat <<EOF
Usage: ${0##*/} [OPTION]...

    -h Display this help and exit
    -t Number of threads to use for thrift serving. Large dataset kept per thread. Default: $THRIFT_THREADS_DEFAULT
    -c Number of threads to use for fanout ranking work. Heavy CPU work. Default: $RANKING_THREADS_DEFAULT
    -s Number of threads to use for task-based serialization cpu work. Default: $SRV_IO_THREADS_DEFAULT
    -l Number of threads to use for load generation in the drivers. Default: $DRIVER_THREADS
    -a When searching for the optimal QPS, automatically adjust the number of client driver threads by
       min(requested_qps / 4, $(nproc) / 5) in each iteration (experimental feature).
    -q Number of QPS to request. If this is present, feedsim will run a fixed-QPS experiment instead of searching
       for a QPS that meets latency target. If multiple comma-separated values are specified, a fixed-QPS experiment
       will be run for each QPS value.
    -d Duration of each load testing experiment, in seconds. Default: 300
    -p Port to use by the LeafNodeRank server and the load drivers. Default: 11222
    -o Result output file name. Default: "feedsim_results.txt"
    -S Store the generated graph to a file (requires a file path)
    -L Load a graph from a file instead of generating one (requires a file path)
    -I Enable timing instrumentation for graph operations (build, store, load)
    -r QPS increase threshold for steady state detection during warmup (in percentage). If specified and greater than zero, warmup continues until
       QPS increase is less than this threshold percentage of the previous QPS.
    -x Maximum number of warmup iterations when using QPS threshold. Default: 10
    -N No retry mode. Skip sleep and PID checking in load test startup, break immediately without retrying.
    -D Drain time in seconds. Time to wait for queue to drain after experiments. Default: 5
EOF
}

cleanup() {
  # remove trap handler
  trap - SIGINT SIGTERM ERR EXIT
  # Check if child process has already been started
  if [ -n "$LEAF_PID" ]; then
    kill -SIGKILL $LEAF_PID || true # Ignore exit status code of kill
  fi
}

msg() {
  echo >&2 -e "${1-}"
}

die() {
  local msg=$1
  local code=${2-1} # default exit status 1
  msg "$msg"
  exit "$code"
}

main() {
    local thrift_threads
    thrift_threads="$THRIFT_THREADS_DEFAULT"

    local ranking_cpu_threads
    ranking_cpu_threads="$RANKING_THREADS_DEFAULT"

    local srv_io_threads
    srv_io_threads="$SRV_IO_THREADS_DEFAULT"

    local driver_threads
    driver_threads="$DRIVER_THREADS"

    local auto_driver_threads
    auto_driver_threads=""

    local fixed_qps
    fixed_qps=""

    local fixed_qps_duration
    fixed_qps_duration="300"

    local warmup_time
    warmup_time="120"

    local port
    port="11222"

    local result_filename
    result_filename="feedsim_results.txt"

    local icache_iterations
    icache_iterations="1600000"

    # Graph storage and loading options
    local store_graph
    store_graph=""

    local load_graph
    load_graph=""

    local instrument_graph
    instrument_graph=""

    local qps_threshold
    qps_threshold=""

    local max_warmup_iterations
    max_warmup_iterations="10"

    local no_retry_mode
    no_retry_mode=""

    local queue_drain_time
    queue_drain_time="5"

    if [ -z "$IS_AUTOSCALE_RUN" ]; then
       echo > $BREPS_LFILE
    fi
    benchreps_tell_state "start"

    while [ $# -ne 0 ]; do
        case $1 in
            -t)
                thrift_threads="$2"
                ;;
            -c)
                ranking_cpu_threads="$2"
                ;;
            -s)
                srv_io_threads="$2"
                ;;
            -l)
                driver_threads="$2"
                ;;
            -a)
                auto_driver_threads="1"
                ;;
            -q)
                fixed_qps="$2"
                ;;
            -d)
                fixed_qps_duration="$2"
                ;;
            -w)
                warmup_time="$2"
                ;;
            -p)
                port="$2"
                ;;
            -o)
                result_filename="$2"
                ;;
            -i)
                icache_iterations="$2"
                ;;
            -S)
                if [ "$2" != "default_do_not_store" ]; then
                    store_graph="--store_graph=$2"
                fi
                ;;
            -L)
                if [ "$2" != "default_do_not_load" ]; then
                    load_graph="--load_graph=$2"
                fi
                ;;
            -I)
                instrument_graph="--instrument_graph"
                ;;
            -r)
                if [[ "$2" -gt 0 ]]; then
                    qps_threshold="$2"
                fi
                ;;
            -x)
                if [[ "$2" -gt 0 ]]; then
                    max_warmup_iterations="$2"
                fi
                ;;
            -N)
                no_retry_mode="1"
                ;;
            -D)
                queue_drain_time="$2"
                ;;
            -h|--help)
                show_help >&2
                exit 1
                ;;
            *)  # end of input
                echo "Unsupported arg '$1'" 1>&2
                break
        esac

        case $1 in
            -t|-c|-s|-d|-p|-q|-o|-w|-i|-l|-S|-L|-r|-x|-D)
                if [ -z "$2" ]; then
                    echo "Invalid option: '$1' requires an argument" 1>&2
                    exit 1
                fi
                shift   # Additional shift for the argument
                ;;
        esac
        shift # pop the previously read argument
    done

    set -u  # Enable unbound variables check from here onwards

    # Bring up services
    # 1. Leaf Node
    # 2. Parent
    # 3. Start Load Driver

    cd "${FEEDSIM_ROOT_SRC}"

    # Starting leaf node service
    monitor_port=$((port-1000))
    MALLOC_CONF=narenas:20,dirty_decay_ms:5000 build/workloads/ranking/LeafNodeRank \
        --port="$port" \
        --monitor_port="$monitor_port" \
        --graph_scale=21 \
        --graph_subset=2000000 \
        --threads="$thrift_threads" \
        --cpu_threads="$ranking_cpu_threads" \
        --timekeeper_threads=2 \
        --io_threads="$EVENTBASE_THREADS_DEFAULT" \
        --srv_threads="$SRV_THREADS_DEFAULT" \
        --srv_io_threads="$srv_io_threads" \
        --num_objects=2000 \
        --graph_max_iters=1 \
        --noaffinity \
        --min_icache_iterations="$icache_iterations" \
        "$store_graph" \
        "$load_graph" \
        "$instrument_graph" >> $BREPS_LFILE 2>&1 &

    LEAF_PID=$!

    # Wait for server to be fully ready using monitoring endpoint
    echo "Waiting for LeafNodeRank server to be ready on monitor port $monitor_port..."
    max_attempts=30
    attempt=0
    while [ $attempt -lt $max_attempts ]; do
        if curl -f -s "http://localhost:$monitor_port/topology" > /dev/null 2>&1; then
            echo "LeafNodeRank server is ready (monitor port responding)"
            break
        fi
        attempt=$((attempt + 1))
        if [ $attempt -eq $max_attempts ]; then
            echo "ERROR: Server failed to become ready within $max_attempts seconds"
            exit 1
        fi
        sleep 1
    done

    # FIXME(cltorres)
    # Skip ParentNode for now, and talk directly to LeafNode
    # ParentNode acts as a simple proxy, and does not influence
    # workload too much. Unfortunately, disabling for now
    # it's not robust at start up, and causes too many failures
    # when trying to create sockets for listening.

    # Start DriverNode
    client_monitor_port="$((monitor_port-1000))"

    # Construct QPS threshold and max iterations parameters if specified
    qps_threshold_args=""
    if [ -n "$qps_threshold" ]; then
        qps_threshold_args="-r $qps_threshold -x $max_warmup_iterations"
    fi

    # Construct no retry mode parameter if specified
    no_retry_args=""
    if [ -n "$no_retry_mode" ]; then
        no_retry_args="-N"
    fi

    if [ -z "$fixed_qps" ] && [ "$auto_driver_threads" != "1" ]; then
        benchreps_tell_state "before search_qps"
        # shellcheck disable=SC2086
        scripts/search_qps.sh -w 15 -f 300 -s 95p:500 $qps_threshold_args $no_retry_args -o "${FEEDSIM_ROOT}/${result_filename}" -- \
            build/workloads/ranking/DriverNodeRank \
                --server "0.0.0.0:$port" \
                --monitor_port "$client_monitor_port" \
                --threads="${driver_threads}" \
                --connections=4
        benchreps_tell_state "after search_qps"
    elif [ -z "$fixed_qps" ] && [ "$auto_driver_threads" = "1" ]; then
        benchreps_tell_state "before search_qps"
        # shellcheck disable=SC2086
        scripts/search_qps.sh -a -w 15 -f 300 -s 95p:500 $qps_threshold_args $no_retry_args -o "${FEEDSIM_ROOT}/${result_filename}" -- \
            build/workloads/ranking/DriverNodeRank \
                --monitor_port "$client_monitor_port" \
                --server "0.0.0.0:$port"
        benchreps_tell_state "after search_qps"
    else
        # Adjust the number of workers according to QPS
        # If driver_threads * connections is too large compared to qps, the driver may not be able
        # to accurately fulfill the requested QPS
        num_connections=4
        num_workers=$((fixed_qps / num_connections))
        if [ "$num_workers" -lt 1 ]; then
            num_workers=1
        elif [ "$num_workers" -gt "$driver_threads" ]; then
            num_workers=$driver_threads
        fi
        benchreps_tell_state "before fixed_qps_exp"
        # shellcheck disable=SC2086
        scripts/search_qps.sh -s 95p -t "$fixed_qps_duration" \
           -m "$warmup_time" \
           -q "$fixed_qps" \
           $qps_threshold_args $no_retry_args \
           -o "${FEEDSIM_ROOT}/${result_filename}" \
           -- build/workloads/ranking/DriverNodeRank \
                --server "0.0.0.0:$port" \
                --monitor_port "$client_monitor_port" \
                --threads="${num_workers}" \
                --connections="${num_connections}"
        benchreps_tell_state "after fixed_qps_exp"
    fi

    sleep "$queue_drain_time" # wait for queue to drain
    kill -SIGINT $LEAF_PID || true > /dev/null # SIGINT so exits cleanly
}

main "$@"

# vim: tabstop=4 shiftwidth=4 expandtab
