#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

"${BENCHPRESS_ROOT}"/install_remove_git.sh remove

GRAPH500_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/graph500"
rm -rf "${GRAPH500_INSTALLATION_PREFIX}"
