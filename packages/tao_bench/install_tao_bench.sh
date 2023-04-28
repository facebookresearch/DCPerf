#!/bin/bash
set -Eeuo pipefail

# FIXME(cltorres): Copy/link bpkgs benchmark contents into the BP_TMP automatically.
BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

# Use the alternative arch-specific installer scripts
ARCH="$(uname -p)"
"${BPKGS_TAO_BENCH_ROOT}"/install_tao_bench_"${ARCH}".sh
exit $?
