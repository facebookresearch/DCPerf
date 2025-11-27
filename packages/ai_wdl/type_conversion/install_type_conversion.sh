#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################
BPKGS_TYPE_CONVERSION_ROOT="$(pwd)/packages/ai_wdl/type_conversion"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TYPE_CONVERSION_ROOT/../../..")"
BENCHMARK_ROOT="${BENCHPRESS_ROOT}/benchmarks"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y libbenchmark-dev cmake

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y epel-release
  dnf install -y google-benchmark-devel cmake
fi

##################### BUILD AND INSTALL #########################
mkdir -p "$BENCHMARK_ROOT/ai_wdl/type_conversion"
cp "${BPKGS_TYPE_CONVERSION_ROOT}/type_conversion_bench.cpp" "${BENCHMARK_ROOT}/ai_wdl/type_conversion/"
cp "${BPKGS_TYPE_CONVERSION_ROOT}/CMakeLists.txt" "${BENCHMARK_ROOT}/ai_wdl/type_conversion/"

cd "$BENCHMARK_ROOT/ai_wdl/type_conversion/"
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=release "../"
make -j "$(nproc)"
cp ./type_conversion_bench "$BENCHMARK_ROOT/ai_wdl/type_conversion/"
