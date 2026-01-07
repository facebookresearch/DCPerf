#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

ARCH="$(uname -m)"
BPKGS_WDL_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
WDL_DIR="${BENCHMARKS_DIR}/wdl_bench"

if [ "$ARCH" = "x86_64" ]; then
    echo "Removing Intel OneMKL"
    MKL_VERSION="2025.3.0.462"
    bash -c "sh $WDL_DIR/wdl_build/intel-onemkl-${MKL_VERSION}_offline.sh -a --action remove --silent --eula accept"
fi

rm -rf "$WDL_DIR"
