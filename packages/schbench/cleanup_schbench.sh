#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BPKGS_SCHBENCH_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_SCHBENCH_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
SCHBENCH_DIR="${BENCHMARKS_DIR}/schbench"

rm -rf "$SCHBENCH_DIR"
