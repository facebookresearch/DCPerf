#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"
cd "${BENCHPRESS_ROOT}"

verify_binaries() {
  test -x benchmarks/halcyon/bin/halcyon
  test -x benchmarks/halcyon/bin/HalcyonServiceMain
  test -x benchmarks/halcyon/bin/checkmark
}

verify_gflags_help() {
  local binary="$1"
  local output
  local status

  if output="$("${binary}" --help 2>&1)"; then
    status=0
  else
    status=$?
  fi
  if [[ ${status} -ne 0 && ${status} -ne 1 ]]; then
    printf '%s\n' "${output}" >&2
    return "${status}"
  fi
  if [[ -z "${output}" ]]; then
    echo "${binary} --help produced no output" >&2
    return 1
  fi
  local normalized="${output,,}"
  if [[ "${normalized}" != *"flags from"* && "${normalized}" != *"usage"* ]]; then
    printf '%s\n' "${output}" >&2
    echo "${binary} --help did not produce gflags usage text" >&2
    return 1
  fi
}

verify_help() {
  local python_path="${BENCHPRESS_ROOT}/benchmarks/halcyon/lib/python:${BENCHPRESS_ROOT}/benchmarks/halcyon/lib/halcyon/python"
  verify_gflags_help benchmarks/halcyon/bin/halcyon
  verify_gflags_help benchmarks/halcyon/bin/HalcyonServiceMain
  PYTHONPATH="${python_path}${PYTHONPATH:+:${PYTHONPATH}}" \
    benchmarks/halcyon/venv/bin/python3 -c \
    'from cea.halcyon.py3.halcyon.thrift_types import PairRole; assert hasattr(PairRole, "SendDiskReads")'
  PYTHONPATH="${python_path}${PYTHONPATH:+:${PYTHONPATH}}" \
    benchmarks/halcyon/venv/bin/python3 \
    benchmarks/halcyon/bin/halcyon_client.py --help >/dev/null
  /usr/bin/python3 packages/halcyon/run.py --help >/dev/null
}

case "${1:-all}" in
  binaries)
    verify_binaries
    ;;
  help)
    verify_help
    ;;
  all)
    verify_binaries
    verify_help
    ;;
  *)
    echo "Usage: $0 [binaries|help|all]" >&2
    exit 2
    ;;
esac
