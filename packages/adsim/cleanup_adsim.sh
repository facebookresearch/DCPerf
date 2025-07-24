#!/bin/bash

# Path to dir with this script
ADSIM_BPKG_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${ADSIM_BPKG_ROOT}/../..")"

rm -rf "${BENCHPRESS_ROOT}/benchmarks/adsim"
