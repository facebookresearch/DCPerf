#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

AI_BENCH_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$AI_BENCH_ROOT/../../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/ai_wdl/pytorch_gemm_gpuless"

rm -rf "$BENCHMARKS_DIR"
