#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################
BPKGS_REBATCH_ROOT="$(pwd)/packages/ai_wdl/rebatch"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_REBATCH_ROOT/../../..")"
BENCHMARK_ROOT="${BENCHPRESS_ROOT}/benchmarks"


# rebatchbench: rebatch overhead benchmark
mkdir -p "$BENCHMARK_ROOT/ai_wdl/rebatch"
cp "${BPKGS_REBATCH_ROOT}/rebatchBench.cpp" "${BENCHMARK_ROOT}/ai_wdl/rebatch/"
cp "${BPKGS_REBATCH_ROOT}/model_a.dist" "${BENCHMARK_ROOT}/ai_wdl/rebatch/"
cp "${BPKGS_REBATCH_ROOT}/model_b.dist" "${BENCHMARK_ROOT}/ai_wdl/rebatch/"

cd "$BENCHMARK_ROOT/ai_wdl/rebatch/"
g++ -o rebatchBench -O2 -lpthread rebatchBench.cpp
