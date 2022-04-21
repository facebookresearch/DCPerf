#!/bin/bash

FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/feedsim"

rm -rf "${FEEDSIM_BENCHMARKS_DIR}"
