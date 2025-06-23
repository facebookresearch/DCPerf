#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BPKGS_REBATCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_REBATCH_ROOT/../..")"
BENCHMARK_ROOT="${BENCHPRESS_ROOT}/benchmarks"

rm -rf "$BENCHMARK_ROOT/rebatch"
