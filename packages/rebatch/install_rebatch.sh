#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################

BPKGS_REBATCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_REBATCH_ROOT/../..")"
BENCHMARK_ROOT="${BENCHPRESS_ROOT}/benchmarks"


# sleepbench: nanosleep overhead benchmark
mkdir -p "$BENCHMARK_ROOT/rebatch"
cp "${BENCHPRESS_ROOT}/packages/rebatch/rebatchBench.cpp" "${BENCHMARK_ROOT}/rebatch/"
cp "${BENCHPRESS_ROOT}/packages/rebatch/model_a.dist" "${BENCHMARK_ROOT}/rebatch/"
cp "${BENCHPRESS_ROOT}/packages/rebatch/model_b.dist" "${BENCHMARK_ROOT}/rebatch/"

cd "$BENCHMARK_ROOT/rebatch/"
g++ -o rebatchBench -O2 -lpthread rebatchBench.cpp
