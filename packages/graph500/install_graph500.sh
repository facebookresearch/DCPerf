#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Installs the Graph500 v3 reference BFS benchmark.
# Upstream: https://github.com/graph500/graph500 (tag 3.0.1).
#
# v3 is MPI-only (no more OpenMP/sequential/XMT kernels) and the build
# requires `mpicc` + an MPI runtime (we use OpenMPI 4.x).
# v3 binaries take a positional SCALE arg, e.g. `mpirun -np 4 ./graph500_reference_bfs 20`.

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

# Install system deps. OpenMPI provides mpicc/mpirun; openmpi-devel ships the
# headers (mpi.h) and the mpicc wrapper. fb-fwdproxy-config gives the git
# clone access to public github via fwdproxy.
dnf install -y fb-fwdproxy-config openmpi openmpi-devel

# Put OpenMPI's mpicc/mpirun on PATH for this script (rpm installs them under
# /usr/lib64/openmpi/bin, which is not in PATH by default).
export PATH="/usr/lib64/openmpi/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib64/openmpi/lib:${LD_LIBRARY_PATH:-}"

GRAPH500_GIT_REPO_URL="https://github.com/graph500/graph500.git"
GRAPH500_GIT_COMMIT_TAG="3.0.1"

BENCHMARKS_DIR="$(pwd)/benchmarks"
mkdir -p "$BENCHMARKS_DIR"

GRAPH500_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/graph500"
GRAPH500_BINARY_PATH="${GRAPH500_INSTALLATION_PREFIX}/graph500_reference_bfs"
GRAPH500_BINARY_SSSP_PATH="${GRAPH500_INSTALLATION_PREFIX}/graph500_reference_bfs_sssp"

rm -rf build
mkdir -p build
cd build/ || exit 1

"${BENCHPRESS_ROOT}"/install_remove_git.sh install

# shellcheck disable=SC2046
git $(fwdproxy-config --git-command git) clone "$GRAPH500_GIT_REPO_URL"
cd graph500/ || exit 1
git checkout -b benchpress "tags/${GRAPH500_GIT_COMMIT_TAG}"

# v3 source layout has a single src/Makefile. The default CFLAGS work, but
# we add -fcommon so the build doesn't fail on GCC >= 10, which defaults to
# -fno-common and rejects the v3 reference code's unguarded tentative
# globals (e.g. `int64_t* column;` declared in csr_reference.h).
make -C src \
    CFLAGS="-Drestrict=__restrict__ -O3 -DGRAPH_GENERATOR_MPI -DREUSE_CSR_FOR_VALIDATION -I../aml -fcommon"

mkdir -p "${GRAPH500_INSTALLATION_PREFIX}"
mv src/graph500_reference_bfs "${GRAPH500_BINARY_PATH}"
mv src/graph500_reference_bfs_sssp "${GRAPH500_BINARY_SSSP_PATH}"
cd ../../ || exit 1

rm -rf build/

echo "Graph500 (v${GRAPH500_GIT_COMMIT_TAG}) installed into ${GRAPH500_INSTALLATION_PREFIX}"
