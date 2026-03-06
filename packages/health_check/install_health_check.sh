#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################

BPKGS_HEALTH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_HEALTH_ROOT/../..")"
HEALTH_ROOT="${BENCHPRESS_ROOT}/benchmarks/health_check"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y iperf3
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y iperf3
fi

cd "$HEALTH_ROOT"
# mm-mem: memory latency and bandwidth benchmark
git clone "https://github.com/pkuwangh/mm-mem.git"
cd mm-mem
./scripts/install_deps.py
make
cd ..

# loaded-latency: memory latency and bandwidth benchmark for ARM
git clone "https://github.com/ARM-software/infra-microbenchmarks"
pushd infra-microbenchmarks/loaded-latency
make
popd

# sleepbench: nanosleep overhead benchmark
mkdir "$HEALTH_ROOT/sleepbench"
cp "${BENCHPRESS_ROOT}/packages/health_check/sleepbench.cpp" "${HEALTH_ROOT}/sleepbench"
cp "${BENCHPRESS_ROOT}/packages/health_check/collect-cpu-util.py" "${HEALTH_ROOT}/sleepbench"
cd "$HEALTH_ROOT/sleepbench"
g++ -o sleepbench -O2 -lpthread sleepbench.cpp

# freq_est: CPU frequency estimation benchmark (ARM only)
if [ "$(uname -m)" = "aarch64" ]; then
  mkdir -p "$HEALTH_ROOT/freq_est"
  cp "${BENCHPRESS_ROOT}/packages/health_check/freq_est.c" "${HEALTH_ROOT}/freq_est"
  cd "$HEALTH_ROOT/freq_est"
  gcc -O3 -Wall -march=armv8-a -o freq_est freq_est.c
fi

cp "${BENCHPRESS_ROOT}/packages/health_check/run.sh" "${HEALTH_ROOT}/run.sh"
