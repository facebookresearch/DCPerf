#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
MW_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$MW_ROOT/../..")"

sudo systemctl stop mariadb
rm -rf "${BENCHPRESS_ROOT}/oss-performance"
rm -rf "${BENCHPRESS_ROOT}/benchmarks/oss_performance_mediawiki"
