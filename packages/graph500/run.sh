#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Wrapper that runs Graph500 v3 reference BFS under mpirun.
#
# Usage:
#   ./run.sh <SCALE> [extra mpirun args...]
#
# Environment variables:
#   NP           Number of MPI ranks (must be a power of 2 in v3 unless
#                you rebuild with -DPROCS_PER_NODE_NOT_POWER_OF_TWO).
#                Default: largest power of 2 <= $(nproc).
#   GRAPH500_BIN Path to graph500 binary. Default uses bfs_sssp if it exists
#                and SKIP_BFS env var to control which kernel runs; otherwise
#                uses graph500_reference_bfs.

set -Eeuo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <SCALE> [extra mpirun args...]" >&2
    exit 1
fi

SCALE="$1"
shift

BENCHPRESS_ROOT="$(pwd)"
GRAPH500_DIR="${BENCHPRESS_ROOT}/benchmarks/graph500"
GRAPH500_BIN="${GRAPH500_BIN:-${GRAPH500_DIR}/graph500_reference_bfs}"

if [[ ! -x "${GRAPH500_BIN}" ]]; then
    echo "Graph500 binary not found at ${GRAPH500_BIN}." >&2
    echo "Run \`./benchpress install graph500_omp_csr\` first." >&2
    exit 1
fi

# OpenMPI from rpm lives outside the default PATH.
export PATH="/usr/lib64/openmpi/bin:${PATH}"
export LD_LIBRARY_PATH="/usr/lib64/openmpi/lib:${LD_LIBRARY_PATH:-}"

# Pick the largest power of 2 <= nproc unless NP is set.
nproc_count="$(nproc)"
if [[ -z "${NP:-}" ]]; then
    NP=1
    while (( NP * 2 <= nproc_count )); do
        NP=$((NP * 2))
    done
fi

echo "Running ${GRAPH500_BIN} SCALE=${SCALE} on ${NP} MPI ranks (host has ${nproc_count} CPUs)"

# Optional knobs (defaults are safe on a clean host):
#   MPIRUN_EXTRA_ARGS  Extra args passed to mpirun (e.g. --allow-run-as-root --oversubscribe)
MPIRUN_EXTRA_ARGS="${MPIRUN_EXTRA_ARGS:-}"
# shellcheck disable=SC2086
exec mpirun ${MPIRUN_EXTRA_ARGS} -np "${NP}" "$@" "${GRAPH500_BIN}" "${SCALE}"
