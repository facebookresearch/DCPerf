#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Cleanup script for the CDN Benchmark (foss_revproxy).
# Kills any running benchmark processes and removes installed binaries.

set -Eeuo pipefail

CDN_PACKAGE_DIR="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${CDN_PACKAGE_DIR}/../../")"
INSTALL_DIR="${BENCHPRESS_ROOT}/benchmarks/cdn_bench"
BUILD_DIR="${CDN_PACKAGE_DIR}/_build"

echo "=== CDN Benchmark Cleanup ==="

# Kill any running benchmark processes
for proc in traffic_client proxy_server content_server; do
  if pgrep -x "$proc" > /dev/null 2>&1; then
    echo "Killing running ${proc} processes..."
    pkill -x "$proc" || true
    sleep 1
    # Force kill if still running
    if pgrep -x "$proc" > /dev/null 2>&1; then
      pkill -9 -x "$proc" || true
    fi
  fi
done

# Remove installed binaries
if [ -d "${INSTALL_DIR}" ]; then
  echo "Removing installed binaries from ${INSTALL_DIR}"
  rm -rf "${INSTALL_DIR}"
fi

# Remove build artifacts
if [ -d "${BUILD_DIR}" ]; then
  echo "Removing build artifacts from ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

# Remove temp stderr files from benchmark runs
rm -f "${CDN_PACKAGE_DIR}/.content_stderr"* "${CDN_PACKAGE_DIR}/.proxy_stderr"* "${CDN_PACKAGE_DIR}/.client_stderr"*

# Remove auto-generated TLS certificates
rm -f "${CDN_PACKAGE_DIR}/.cdn_bench_cert.pem" "${CDN_PACKAGE_DIR}/.cdn_bench_key.pem"
rm -f /tmp/cdn_bench_tls_cert.pem /tmp/cdn_bench_tls_key.pem

# Remove log file
if [ -f "${CDN_PACKAGE_DIR}/cdn_bench_run.log" ]; then
  rm -f "${CDN_PACKAGE_DIR}/cdn_bench_run.log"
fi

echo "=== CDN Benchmark Cleanup Complete ==="
