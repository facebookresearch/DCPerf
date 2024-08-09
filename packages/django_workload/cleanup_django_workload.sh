#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

DJANGO_PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${DJANGO_PKG_ROOT}/../..")"
DJANGO_BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/django_workload"

# TODO: Uninstall siege here!
rm -rf "${DJANGO_BENCHMARKS_DIR}"
# Kill Cassandra process
pkill java || true
# Kill memcache processes
pkill memcache || true
# Kill uWSGI master process
kill $(ps aux | grep -i '[u]wsgi master' | awk '{print $2}') || true
