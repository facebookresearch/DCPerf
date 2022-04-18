#!/bin/bash

BENCHPRESS_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
TAO_BENCH_DIR="${BENCHMARKS_DIR}/tao_bench"

rm -rf "$TAO_BENCH_DIR"
