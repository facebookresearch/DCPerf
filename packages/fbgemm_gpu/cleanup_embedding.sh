#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


AI_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$AI_BENCH_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/fbgemm_embedding"
MEMCACHE_BENCH_DIR="${BENCHMARKS_DIR}/tbe_inference_benchmark"

rm -rf "$MEMCACHE_BENCH_DIR"
