#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"
INSTALL_ROOT="${BENCHPRESS_ROOT}/benchmarks/halcyon"

if [[ "${1:-}" == "--filesets" ]]; then
  exec python3 "${SCRIPT_DIR}/run.py" cleanup-filesets "${@:2}"
fi

# Normal Benchpress cleanup intentionally leaves marker-owned filesets intact.
if [[ -d "${INSTALL_ROOT}" ]]; then
  rm -rf -- "${INSTALL_ROOT}"
fi
