#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -euo pipefail

BPKGS_WDL_ROOT="$(dirname "$(readlink -f -- "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
WDL_ROOT="${BENCHPRESS_ROOT}/benchmarks/wdl_bench"

# shellcheck disable=SC1091
source "$BPKGS_WDL_ROOT"/common.sh

if [ "$ARCH" = "x86_64" ] && [ -f "$WDL_ROOT/wdl_build/intel-onemkl-${MKL_VERSION}_offline.sh" ]; then
    echo "Removing Intel OneMKL"
    bash -c "sh $WDL_ROOT/wdl_build/intel-onemkl-${MKL_VERSION}_offline.sh -a --action remove --silent --eula accept"
fi

rm -rf "$WDL_ROOT"
