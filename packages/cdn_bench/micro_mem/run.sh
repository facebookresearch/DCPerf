#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

# Get the directory where this script is located
MEM_MICRO_DIR="$(dirname "$(readlink -f "$0")")"
LOG_FILE="${MEM_MICRO_DIR}/stream_run.log"

# Clear previous log file
true > "$LOG_FILE"

# Redirect all output to both stdout and log file
exec > >(tee -a "$LOG_FILE") 2>&1

echo "MEM"

# Parse arguments with defaults
ARRAY_SIZE="${1:-201326592}"
NTIMES="${2:-100}"

# Source file and binary name
SRC="stream.c"
BIN="stream"

# Change to mem_micro directory
cd "$MEM_MICRO_DIR" || exit 1

# Detect number of physical cores for OpenMP
NUM_THREADS=$(nproc)
echo "Detected $NUM_THREADS available CPUs for OpenMP parallelization"

# Compile the STREAM benchmark with OpenMP support
echo "Compiling $SRC with OpenMP, stream array size = $ARRAY_SIZE and iterations = $NTIMES..."
gcc -O3 -fopenmp -mcmodel=large -DSTREAM_ARRAY_SIZE="$ARRAY_SIZE" -DNTIMES="$NTIMES" "$SRC" -o "$BIN"

# Check compilation success
if [[ ! -x "$BIN" ]]; then
  echo "ERROR: Compilation failed or binary not found!"
  exit 1
fi

# Set OpenMP environment variables for optimal performance
export OMP_NUM_THREADS="$NUM_THREADS"
export OMP_PROC_BIND=spread
export OMP_PLACES=threads

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
echo "OpenMP Configuration:"
echo "  - OMP_NUM_THREADS: $OMP_NUM_THREADS"
echo "  - OMP_PROC_BIND: $OMP_PROC_BIND"
echo "  - OMP_PLACES: $OMP_PLACES"
echo "====================================================================="

# Run STREAM with NUMA interleaving for optimal memory access
if command -v numactl &> /dev/null; then
  echo "Running with numactl --interleave=all for NUMA optimization"
  numactl --interleave=all ./"$BIN" 2>&1
else
  echo "WARNING: numactl not found, running without NUMA optimization"
  ./"$BIN" 2>&1
fi
echo ""

# Get memory information
echo "Total Memory: $(free -h | awk '/^Mem:/ {print $2}')"
printf "\n"
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
