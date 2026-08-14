#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

# FIXME(cltorres): Copy/link bpkgs benchmark contents into the BP_TMP automatically.
BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

# Use the alternative arch-specific installer scripts
# CentOS 10 (and some container kernels) return "unknown" for `uname -p`,
# which would try to run install_tao_bench_unknown.sh. Fall back to
# `uname -m` and normalize to the script naming (x86_64 / aarch64).
canon_arch="$(uname -m)"
case "${canon_arch}" in
  x86_64|amd64) ARCH="x86_64" ;;
  aarch64|arm64) ARCH="aarch64" ;;
  *) ARCH="${canon_arch}" ;;
esac
"${BPKGS_TAO_BENCH_ROOT}"/install_tao_bench_"${ARCH}".sh
exit $?
