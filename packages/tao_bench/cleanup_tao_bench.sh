#!/bin/bash

BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TAO_BENCH_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
TAO_BENCH_DIR="${BENCHMARKS_DIR}/tao_bench"

rm -rf "$TAO_BENCH_DIR"
