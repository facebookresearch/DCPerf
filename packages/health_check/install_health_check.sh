#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################

BPKGS_HEATH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_HEATH_ROOT/../..")"
HEALTH_ROOT="${BENCHPRESS_ROOT}/benchmarks/health_check"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

# CentOS containers run as root but with root locked and no sudo PAM
# setup until the workflow adds it — `sudo` fails there with
# "account validation failure, is your account locked?" on dnf installs.
SUDO=""
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  SUDO="sudo"
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  if sudo -n true 2>/dev/null; then
    SUDO="sudo"
  else
    SUDO=""
  fi
fi

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  ${SUDO} apt install -y iperf3 libnuma-dev libboost-program-options-dev libboost-system-dev
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  ${SUDO} dnf install -y iperf3 numactl-devel boost-devel
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
cp "${BENCHPRESS_ROOT}/packages/health_check/run.sh" "${HEALTH_ROOT}/run.sh"
