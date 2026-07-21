#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Installs the GAP Benchmark Suite (gapbs) from upstream
# https://github.com/sbeamer/gapbs (tag v1.1).
#
# Produces six benchmark binaries (bc, bfs, cc, pr, sssp, tc) plus the
# `converter` utility in ${BENCHPRESS_ROOT}/benchmarks/gapbs/. Jobs can
# either point at the synthetic Kronecker generator (`-g <SCALE>`) or at a
# pre-built `.sg` / `.wsg` / `.sgU` graph file via `-f`. Building large
# real-world graphs (e.g. twitter_rv) is intentionally NOT done here -
# do it in a separate one-off step to keep this script idempotent and
# cheap to re-run.

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

GAPBS_GIT_REPO_URL="https://github.com/sbeamer/gapbs.git"
GAPBS_GIT_COMMIT_TAG="v1.1"

# Install system deps. fb-fwdproxy-config gives `git clone` access to public
# github via fwdproxy on Meta dev servers / sandboxes.
dnf install -y fb-fwdproxy-config gcc gcc-c++ libstdc++

BENCHMARKS_DIR="$(pwd)/benchmarks"
mkdir -p "${BENCHMARKS_DIR}"

GAPBS_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/gapbs"

rm -rf build
mkdir -p build
cd build/ || exit 1

"${BENCHPRESS_ROOT}"/install_remove_git.sh install

# shellcheck disable=SC2046
git $(fwdproxy-config --git-command git) clone "${GAPBS_GIT_REPO_URL}"
cd gapbs/ || exit 1
git checkout -b benchpress "tags/${GAPBS_GIT_COMMIT_TAG}"

make -j"$(nproc)"

mkdir -p "${GAPBS_INSTALLATION_PREFIX}"
mv bc bfs cc pr sssp tc converter "${GAPBS_INSTALLATION_PREFIX}"
cd ../../ || exit 1

rm -rf build/

# Copy the multi-instance runner + graph generator alongside the kernels so
# benchpress can invoke them from ./benchmarks/gapbs/.
for f in run-gapbs-multi.sh generate_graph.sh; do
    cp "${SCRIPT_DIR}/${f}" "${GAPBS_INSTALLATION_PREFIX}/${f}"
    chmod u+x "${GAPBS_INSTALLATION_PREFIX}/${f}"
done

echo "GAP benchmark suite (${GAPBS_GIT_COMMIT_TAG}) installed into ${GAPBS_INSTALLATION_PREFIX}"
