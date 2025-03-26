#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeo pipefail
#trap SIGINT SIGTERM ERR EXIT


HEALTH_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-r server|client] [-c clients]
    -h, --help        Display this help and exit
    -r, --role        Specify role (server or client)
    -c, --clients     Specify clients
EOF
}

# Parse command-line options using getopt
TEMP=$(getopt -o hr:c: --long help,role:,clients: -- "$@")
# Set positional parameters to parsed options
eval set -- "$TEMP"
while true; do
  case "$1" in
    -h | --help)
      show_help
      exit 0
      ;;
    -r | --role)
      role="$2"
      shift 2
      ;;
    -c | --clients)
      clients="$2"
      shift 2
      ;;
    --)
      shift
      break
      ;;
  esac
done

if [ "$role" = "client" ]; then
  iperf3 -s &
fi


if [ "$role" = "server" ]; then
  IFS=',' read -r -a client_array <<< "$clients"
  for client in "${client_array[@]}"; do
    ping -c 10 "$client"
  done
  for client in "${client_array[@]}"; do
    iperf3 -c "$client" -P4
  done
  cpus=$(nproc)
  workers=$(echo "scale=0; $cpus * 2.5" | bc)
  python3 "$HEALTH_ROOT"/sleepbench/collect-cpu-util.py "$HEALTH_ROOT"/sleepbench/sleepbench "$workers" 30
  if [ "$(uname -p)" = "aarch64" ]; then
    NPROC="$(nproc)"
    BW_CORES=""
    for ((i=0; i<NPROC-1; i+=2)); do
      BW_CORES="${BW_CORES}-B${i} "
    done
    pushd "${HEALTH_ROOT}/infra-microbenchmarks/loaded-latency" || exit 1
    ./sweep.finedelay.sh ${BW_CORES} > /tmp/bw-lat.txt
    ./summarize.sh /tmp/bw-lat.txt > /tmp/bw-lat.tsv
    ./run-200mb.latency-only.sh > /tmp/latency.txt
  else
    python3 "$HEALTH_ROOT"/mm-mem/scripts/run_cpu_micro.py
  fi
fi
