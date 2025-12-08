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

# Collect system metadata before benchmark
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
./"$BIN" 2>&1
echo ""

# Get memory information
echo "Total Memory: $(free -h | awk '/^Mem:/ {print $2}')\n"
echo "====================================================================="
echo "Memory Configuration"
echo "====================================================================="
echo "Memory Channels:"
dmidecode -t memory 2>/dev/null | grep -E 'Locator|Size|Speed|Type:' | head -20 || echo "dmidecode not available"
echo ""

# Run STREAM benchmark with comprehensive perf monitoring
echo "====================================================================="
echo "Running STREAM Benchmark with Performance Monitoring"
echo "====================================================================="
echo ""

# Run perf stat with extended memory and hardware counters(Captured with built in perfstat counters)

# Post-benchmark metrics collection
echo ""
echo "====================================================================="
echo "Post-Benchmark Metrics"
echo "====================================================================="

# Memory Allocation/Deallocation Patterns
echo ""
echo "Peak Memory Usage (High-Water Mark):"
echo "RSS Peak: $(grep VmHWM /proc/$$/status 2>/dev/null || echo 'Not available')"
echo "Current Memory Usage:"
free -h
echo ""

# Resource Utilization and Scaling (Captured in mpstat.log)

# NUMA latency matrix if numactl is available
echo "====================================================================="
echo "NUMA Latency Matrix"
echo "====================================================================="
if command -v numactl &> /dev/null; then
    echo "NUMA distances (lower is better):"
    numactl --hardware | grep -A 100 "node distances"
else
    echo "numactl not available for NUMA latency measurement"
fi
echo ""

# Cache hierarchy information
echo "====================================================================="
echo "Cache Hierarchy"
echo "====================================================================="
lscpu | grep -E 'cache|Cache'
echo ""

# Page size information
echo "====================================================================="
echo "Memory Page Configuration"
echo "====================================================================="
echo "Default page size: $(getconf PAGESIZE) bytes"
if [ -d /sys/kernel/mm/hugepages ]; then
    echo "Huge pages configuration:"
    for hp in /sys/kernel/mm/hugepages/hugepages-*; do
        if [ -d "$hp" ]; then
            size=$(basename "$hp" | sed 's/hugepages-//')
            nr=$(cat "$hp/nr_hugepages" 2>/dev/null)
            free=$(cat "$hp/free_hugepages" 2>/dev/null)
            echo "  $size: $nr total, $free free"
        fi
    done
else
    echo "Huge pages not configured"
fi
echo ""

echo "====================================================================="
echo "Benchmark Execution Complete"
echo "====================================================================="
