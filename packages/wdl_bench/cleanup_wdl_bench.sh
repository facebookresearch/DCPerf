#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BPKGS_WDL_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
WDL_DIR="${BENCHMARKS_DIR}/wdl_bench"

rm -rf "$WDL_DIR"
