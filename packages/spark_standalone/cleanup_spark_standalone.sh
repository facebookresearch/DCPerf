#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
SPARKBENCH_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${SPARKBENCH_ROOT}/../..")"

rm -rf "${BENCHPRESS_ROOT}/benchmarks/spark_standalone"
