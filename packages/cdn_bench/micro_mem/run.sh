#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
echo "MEM"
set -Eeuo pipefail

MEM_MICRO_DIR="$(dirname "$(readlink -f "$0")")"

# Parse arguments with defaults
ARRAY_SIZE="${1:-201326592}"
NTIMES="${2:-100}"
LOG_FILE="${3:-stream_run.log}"

# Source file and binary name
SRC="stream.c"
BIN="stream"

# Change to mem_micro directory
cd "$MEM_MICRO_DIR" || exit 1

# Check if stream.c exists
if [ ! -f "$SRC" ]; then
  echo "ERROR: $SRC not found. Please run install_mem_micro.sh first."
  exit 1
fi

# Compile the STREAM benchmark
echo "Compiling $SRC with stream array size = $ARRAY_SIZE and iterations =$NTIMES..."
gcc -O -mcmodel=large -DSTREAM_ARRAY_SIZE="$ARRAY_SIZE" -DNTIMES="$NTIMES" "$SRC" -o "$BIN"

# Check compilation success
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: Compilation failed or binary not found!"
  exit 1
fi

# Run perf stat and output to stdout
echo "MEM"
echo "====================================================================="
echo "STREAM Benchmark Execution"
echo "====================================================================="
echo "Script: $0"
echo "Execution Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Arguments Passed:"
echo "  - STREAM_ARRAY_SIZE: $ARRAY_SIZE"
echo "  - NTIMES: $NTIMES"
echo "====================================================================="
echo ""
perf stat -e cycles,instructions,cache-references,cache-misses,LLC-load-misses,LLC-store-misses ./"$BIN" 2>&1
