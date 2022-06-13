#!/bin/bash

set -Eeo pipefail
trap cleanup SIGINT SIGTERM ERR EXIT

BREPS_LFILE=/tmp/feedsim_log.txt

function benchreps_tell_state () {
    date +"${1} %Y-%m-%d_%T" >> $BREPS_LFILE
}


# Assumes run.sh is copied to the benchmark directory
#  ${BENCHPRESS_ROOT}/feedsim/run.sh

# Function for BC
BC_MAX_FN='define max (a, b) { if (a >= b) return (a); return (b); }'

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
FEEDSIM_ROOT_SRC="${FEEDSIM_ROOT}/src"

THRIFT_THREADS_DEFAULT="$(nproc)" # Scale with logial cpus
RANKING_THREADS_DEFAULT="$(( $(nproc) * 7/20))"  # 7/20 is 0.35 cpu factor
EVENTBASE_THREADS_DEFAULT=4  # 4 should suffice. Tune up if threads are saturated.
SRV_THREADS_DEFAULT=8        # 8 should also suffice for most purposes
SRV_IO_THREADS_DEFAULT="$(nproc)" # Scale with logical cpus
DRIVER_THREADS="$(echo "scale=2; $(nproc) / 5.0 + 0.5 " | bc )"  # Driver threads, rounds nearest.
DRIVER_THREADS="${DRIVER_THREADS%.*}"  # Truncate decimal fraction.
DRIVER_THREADS="$(echo "${BC_MAX_FN}; max(${DRIVER_THREADS:-0}, 4)" | bc )" # At least 4 threads.


show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-t <thrift_threads>] [-c <ranking_cpu_threads>]
                [-e <io_threads>]

    -h Display this help and exit
    -t Number of threads to use for thrift serving. Large dataset kept per thread. Default: $THRIFT_THREADS_DEFAULT
    -c Number of threads to use for fanout ranking work. Heavy CPU work. Default: $RANKING_THREADS_DEFAULT
    -s Number of threads to use for task-based serialization cpu work. Default: $SRV_IO_THREADS_DEFAULT
EOF
}

cleanup() {
  trap - SIGINT SIGTERM ERR EXIT

  pkill -2 LeafNodeRank || true # Ignore exit status code of pkill
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


    echo > $BREPS_LFILE
    benchreps_tell_state "start"

    while :; do
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
            -h)
                show_help >&2
                exit 1
                ;;
            *)  # end of input
                break
        esac

        case $1 in
            -t|-c|-s)
                if [ -z "$2" ]; then
                    echo "Invalid option: $1 requires an argument" 1>&2
                    exit 1
                fi
                shift   # Additional shift for the argument
                ;;
        esac
        shift
    done

    set -u  # Enable unbound variables check from here onwards

    # Bring up services
    # 1. Leaf Node
    # 2. Parent
    # 3. Start Load Driver

    cd "${FEEDSIM_ROOT_SRC}"

    # Starting leaf node service
    MALLOC_CONF=narenas:20,dirty_decay_ms:5000 build/workloads/ranking/LeafNodeRank \
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
        --min_icache_iterations=1600000 &

    LEAF_PID=$!

    # FIXME(cltorres)
    # Remove sleep, expose an endpoint or print a message to notify service is ready
    sleep 90

    # FIXME(cltorres)
    # Skip ParentNode for now, and talk directly to LeafNode
    # ParentNode acts as a simple proxy, and does not influence
    # workload too much. Unfortunately, disabling for now
    # it's not robust at start up, and causes too many failures
    # when trying to create sockets for listening.

    # Start DriverNode
    benchreps_tell_state "before search_qps"
    scripts/search_qps.sh -w 15 -f 300 -s 95p:500 -o "${FEEDSIM_ROOT}/feedsim_results.txt" -- \
        build/workloads/ranking/DriverNodeRank \
            --server 0.0.0.0:11222 \
            --threads="${DRIVER_THREADS}" \
            --connections=4
    benchreps_tell_state "after search_qps"

    sleep 5 # wait for queue to drain
    kill -SIGINT $LEAF_PID  # SIGINT so exits cleanly
}

main "$@"
