#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

echo "Cleaning up UcacheBench..."

# Get script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
BENCHPRESS_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
UCACHE_BENCH_DIR="$BENCHPRESS_ROOT/benchmarks/ucache_bench"

echo "UcacheBench directory: $UCACHE_BENCH_DIR"

# Clean up benchmark directory and binaries
if [ -d "$UCACHE_BENCH_DIR" ]; then
    echo "Removing UcacheBench benchmark directory..."
    rm -rf "$UCACHE_BENCH_DIR"
    echo "Removed $UCACHE_BENCH_DIR"
else
    echo "UcacheBench directory not found: $UCACHE_BENCH_DIR"
fi

# Clean up any temporary files (SSD cache files, etc.)
echo "Cleaning up temporary UcacheBench files..."
rm -f /tmp/ucachebench_ssd* 2>/dev/null || true
rm -f /tmp/ucachebench_*.log 2>/dev/null || true

echo "UcacheBench cleanup completed successfully!"
