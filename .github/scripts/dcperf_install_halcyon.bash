#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

bash "${BENCHPRESS_ROOT}/scripts/install-deps.sh"
bash "${BENCHPRESS_ROOT}/packages/halcyon/install_halcyon.sh"
