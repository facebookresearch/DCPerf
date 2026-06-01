#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Wrapper that runs Silo's dbtest binary. Benchpress invokes this via the
# `path:` entry in benchmarks_internal.yml; all args from the job config are
# passed through.
#
# Convenience: `--num-threads 0` (or `--num-threads=0`) is rewritten to the
# host's nproc so job configs can ask for "all cores" portably without
# hard-coding a value. dbtest itself does not accept 0.
#
# Usage:
#   ./run.sh <dbtest args...>
#
# Environment variables:
#   SILO_NUMA_NODE  If set (e.g. "0" or "0,1"), wrap dbtest in
#                   `numactl --membind=$NODE --cpunodebind=$NODE`.
#   SILO_CPUS       If set (e.g. "0-15" or "0,2,4,6"), wrap dbtest in
#                   `taskset -c $SILO_CPUS`. Applied after numactl.
#   SILO_BIN        Override path to the dbtest binary.

set -Eeuo pipefail

BENCHPRESS_ROOT="$(pwd)"
SILO_DIR="${BENCHPRESS_ROOT}/benchmarks/silo"
SILO_BIN="${SILO_BIN:-${SILO_DIR}/dbtest}"

if [[ ! -x "${SILO_BIN}" ]]; then
    echo "Silo dbtest binary not found at ${SILO_BIN}." >&2
    echo "Run \`./benchpress_cli.py install silo_default\` (or any other silo_* job) first." >&2
    exit 1
fi

nproc_count="$(nproc)"

args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --num-threads)
            if [[ $# -lt 2 ]]; then
                echo "Error: --num-threads requires a value (use 0 to mean nproc)" >&2
                exit 2
            fi
            args+=("--num-threads")
            if [[ "$2" == "0" ]]; then
                args+=("${nproc_count}")
            else
                args+=("$2")
            fi
            shift 2
            ;;
        --num-threads=0)
            # Other --num-threads=N forms (N != 0) fall through to the
            # default `*` case below and are passed to dbtest verbatim.
            args+=("--num-threads=${nproc_count}")
            shift
            ;;
        *)
            args+=("$1")
            shift
            ;;
    esac
done

cmd=("${SILO_BIN}" "${args[@]}")

if [[ -n "${SILO_CPUS:-}" ]]; then
    cmd=(taskset -c "${SILO_CPUS}" "${cmd[@]}")
fi

if [[ -n "${SILO_NUMA_NODE:-}" ]]; then
    cmd=(numactl "--membind=${SILO_NUMA_NODE}" "--cpunodebind=${SILO_NUMA_NODE}" "${cmd[@]}")
fi

echo "Running: ${cmd[*]}" >&2
exec "${cmd[@]}"
