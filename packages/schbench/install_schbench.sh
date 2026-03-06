#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -euo pipefail

PKG_SCHBENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

rm -rf build
mkdir -p build
pushd build
    # make schbench
    # shellcheck disable=SC2046
    git clone https://kernel.googlesource.com/pub/scm/linux/kernel/git/mason/schbench

    pushd schbench
        make -j"${BP_CPUS:-$(nproc)}"
        # move the binary to the install dir
        install -m755 -D schbench "${PKG_SCHBENCH_ROOT}/bin/schbench"
    popd
popd

# destroy the build directory
rm -rf build

echo "schbench installed to ${PKG_SCHBENCH_ROOT}/bin/schbench"
