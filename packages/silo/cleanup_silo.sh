#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Removes the Silo dbtest binary, the silo_build tree (which holds the
# dynamically-linked masstree .so), and uninstalls git via the shared
# install_remove_git.sh helper (matches the graph500 cleanup pattern).

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

"${BENCHPRESS_ROOT}"/install_remove_git.sh remove

BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
SILO_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/silo"

rm -rf "${SILO_INSTALLATION_PREFIX}"
rm -rf "${BENCHPRESS_ROOT}/silo_build"

echo "Silo benchmark uninstalled from ${SILO_INSTALLATION_PREFIX}"
