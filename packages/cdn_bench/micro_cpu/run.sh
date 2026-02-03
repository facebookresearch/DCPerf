#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

# Output benchmark type identifier for parser
echo "CPU"

# shellcheck disable=SC2034
CPU_MICRO_DIR="$(dirname "$(readlink -f "$0")")"

# Default values
TEST_TYPE="cpu"
CPU_MAX_PRIME=10000
THREADS=0
TIME=60
MEMORY_BLOCK_SIZE="4K"
MEMORY_TOTAL_SIZE="100G"
MEMORY_OPER="read"
MEMORY_ACCESS_MODE="seq"
GOVERNOR="performance"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --test-type=*)
            TEST_TYPE="${1#*=}"
            shift
            ;;
        --cpu-max-prime=*)
            CPU_MAX_PRIME="${1#*=}"
            shift
            ;;
        --threads=*)
            THREADS="${1#*=}"
            shift
            ;;
        --time=*)
            TIME="${1#*=}"
            shift
            ;;
        --memory-block-size=*)
            MEMORY_BLOCK_SIZE="${1#*=}"
            shift
            ;;
        --memory-total-size=*)
            MEMORY_TOTAL_SIZE="${1#*=}"
            shift
            ;;
        --memory-oper=*)
            MEMORY_OPER="${1#*=}"
            shift
            ;;
        --memory-access-mode=*)
            MEMORY_ACCESS_MODE="${1#*=}"
            shift
            ;;
        --governor=*)
            GOVERNOR="${1#*=}"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# If threads is 0 or empty, use nproc (auto-detect)
if [[ "$THREADS" == "0" || -z "$THREADS" ]]; then
    THREADS=$(nproc)
fi

# Set CPU governor for consistent results
echo "====================================================================="
echo "CPU Governor Configuration"
echo "====================================================================="
if command -v cpupower &> /dev/null; then
    echo "Setting CPU governor to: $GOVERNOR"
    sudo cpupower frequency-set -g "$GOVERNOR" 2>/dev/null || echo "Warning: Could not set CPU governor (requires root)"
    cpupower frequency-info 2>/dev/null | grep -E 'governor|driver' || true
else
    echo "cpupower not available - skipping governor configuration"
fi
echo ""

# Check sysbench availability
if ! command -v sysbench &> /dev/null; then
    echo "ERROR: sysbench not found. Please install sysbench."
    exit 1
fi

echo "Sysbench version: $(sysbench --version)"
echo ""

# Run the appropriate benchmark
echo "====================================================================="
echo "Running Sysbench Benchmark"
echo "====================================================================="

case $TEST_TYPE in
    cpu)
        echo "Test: CPU"
        echo "Parameters:"
        echo "  - cpu-max-prime: $CPU_MAX_PRIME"
        echo "  - threads: $THREADS"
        echo "  - time: $TIME seconds"
        echo ""
        sysbench cpu \
            --cpu-max-prime="$CPU_MAX_PRIME" \
            --threads="$THREADS" \
            --time="$TIME" \
            run
        ;;
    memory)
        echo "Test: Memory"
        echo "Parameters:"
        echo "  - memory-block-size: $MEMORY_BLOCK_SIZE"
        echo "  - memory-total-size: $MEMORY_TOTAL_SIZE"
        echo "  - memory-oper: $MEMORY_OPER"
        echo "  - memory-access-mode: $MEMORY_ACCESS_MODE"
        echo "  - threads: $THREADS"
        echo ""
        sysbench memory \
            --memory-block-size="$MEMORY_BLOCK_SIZE" \
            --memory-total-size="$MEMORY_TOTAL_SIZE" \
            --memory-oper="$MEMORY_OPER" \
            --memory-access-mode="$MEMORY_ACCESS_MODE" \
            --threads="$THREADS" \
            run
        ;;
    cdn_edge)
        echo "Test: CDN Edge Host Profile"
        echo "Parameters:"
        echo "  - cpu-max-prime: 5000 (small working sets)"
        echo "  - threads: $THREADS"
        echo "  - time: $TIME seconds"
        echo ""
        sysbench cpu \
            --cpu-max-prime=5000 \
            --threads="$THREADS" \
            --time="$TIME" \
            run
        ;;
    read_optimized)
        echo "Test: Read-Optimized Drives Profile"
        echo "Parameters:"
        echo "  - memory-block-size: 4K"
        echo "  - memory-total-size: 100G"
        echo "  - memory-oper: read"
        echo "  - memory-access-mode: $MEMORY_ACCESS_MODE"
        echo "  - threads: $THREADS"
        echo ""
        sysbench memory \
            --memory-block-size=4K \
            --memory-total-size=100G \
            --memory-oper=read \
            --memory-access-mode="$MEMORY_ACCESS_MODE" \
            --threads="$THREADS" \
            run
        ;;
    caching)
        echo "Test: Large Caching Machines Profile"
        echo "Parameters:"
        echo "  - memory-block-size: 64 (small blocks)"
        echo "  - memory-total-size: 50G"
        echo "  - memory-access-mode: rnd (random)"
        echo "  - threads: $THREADS"
        echo ""
        sysbench memory \
            --memory-block-size=64 \
            --memory-total-size=50G \
            --memory-access-mode=rnd \
            --threads="$THREADS" \
            run
        ;;
    object_storage)
        echo "Test: Object Storage Profile"
        echo "Parameters:"
        echo "  - cpu-max-prime: 20000 (large primes)"
        echo "  - threads: $THREADS"
        echo "  - time: $TIME seconds"
        echo ""
        sysbench cpu \
            --cpu-max-prime=20000 \
            --threads="$THREADS" \
            --time="$TIME" \
            run
        ;;
    networking)
        echo "Test: High Networking Workloads Profile"
        echo "Parameters:"
        echo "  - cpu-max-prime: 3000 (small tasks)"
        echo "  - threads: $THREADS"
        echo "  - time: $TIME seconds"
        echo ""
        sysbench cpu \
            --cpu-max-prime=3000 \
            --threads="$THREADS" \
            --time="$TIME" \
            run
        ;;
    *)
        echo "ERROR: Unknown test type: $TEST_TYPE"
        echo "Valid options: cpu, memory, cdn_edge, read_optimized, caching, object_storage, networking"
        exit 1
        ;;
esac

echo ""
