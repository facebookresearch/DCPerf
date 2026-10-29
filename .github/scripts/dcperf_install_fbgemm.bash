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

bash scripts/install-deps.sh
/usr/bin/python3 benchpress_cli.py -b ai install fbgemm_embedding_a_single
