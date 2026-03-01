#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

CPU_MICRO_DIR="$(dirname "$(realpath "$0")")"
LOG_FILE="${CPU_MICRO_DIR}/cpu_run.log"
YAML_FILE="${CPU_MICRO_DIR}/stress_ng_metrics.yaml"

# Redirect all output to both console and log file
exec > >(tee -a "$LOG_FILE") 2>&1

# Clear log file at start
true > "$LOG_FILE"

echo "CPU"
echo "Running CPU Micro"

# Default values
STRESSOR="cpu"
WORKERS=0
TIMEOUT=60
CPU_METHOD="all"
MATRIX_SIZE=128
VM_BYTES="256M"
GOVERNOR="performance"
TASKSET=""
VERIFY=0
AGGRESSIVE=0

# Parse command line arguments (--option=value format)
for arg in "$@"; do
    case "$arg" in
        --stressor=*) STRESSOR="${arg#*=}" ;;
        --workers=*) WORKERS="${arg#*=}" ;;
        --timeout=*) TIMEOUT="${arg#*=}" ;;
        --cpu-method=*) CPU_METHOD="${arg#*=}" ;;
        --matrix-size=*) MATRIX_SIZE="${arg#*=}" ;;
        --vm-bytes=*) VM_BYTES="${arg#*=}" ;;
        --governor=*) GOVERNOR="${arg#*=}" ;;
        --taskset=*) TASKSET="${arg#*=}" ;;
        --verify=*) VERIFY="${arg#*=}" ;;
        --aggressive=*) AGGRESSIVE="${arg#*=}" ;;
    esac
done

# If workers is 0 or empty, use nproc (auto-detect)
if [[ "$WORKERS" == "0" || -z "$WORKERS" ]]; then
    WORKERS=$(nproc)
fi

# Check stress-ng availability
if ! command -v stress-ng &> /dev/null; then
    echo "ERROR: stress-ng is not installed. Run install_cpu_micro.sh first."
    exit 1
fi

# Set CPU governor for consistent results
echo "====================================================================="
echo "CPU Governor Configuration"
echo "====================================================================="
if command -v cpupower &> /dev/null; then
    echo "Setting CPU governor to: ${GOVERNOR}"
    sudo cpupower frequency-set -g "${GOVERNOR}" 2>/dev/null || echo "Warning: Could not set CPU governor (requires root)"
    cpupower frequency-info 2>/dev/null | grep -E 'governor|driver' || true
else
    echo "cpupower not available - skipping governor configuration"
fi
echo ""

# Collect system metadata before benchmark
echo "====================================================================="
echo "CPU Micro Benchmark (stress-ng)"
echo "====================================================================="
echo "Script: $0"
echo "Execution Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "stress-ng Version: $(stress-ng --version 2>&1 | head -1)"
echo ""

echo "====================================================================="
echo "System Information"
echo "====================================================================="
echo "Hostname: $(hostname)"
echo "Kernel: $(uname -r)"
echo "Architecture: $(uname -m)"
echo "CPU Model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
echo "Physical Cores: $(grep -c '^processor' /proc/cpuinfo)"
echo "NUMA Nodes: $(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l)"
echo ""

# CPU frequency info
echo "====================================================================="
echo "CPU Frequency Information"
echo "====================================================================="
if command -v cpupower &> /dev/null; then
    cpupower frequency-info 2>/dev/null | grep -E 'current CPU|hardware limits|governor' || true
else
    cat /proc/cpuinfo | grep -m1 'cpu MHz' || true
fi
echo ""

echo "====================================================================="
echo "Configuration"
echo "====================================================================="
echo "Stressor: ${STRESSOR}"
echo "Workers: ${WORKERS}"
echo "Timeout: ${TIMEOUT}s"
case "$STRESSOR" in
    cpu)
        echo "CPU Method: ${CPU_METHOD}"
        ;;
    matrix)
        echo "Matrix Size: ${MATRIX_SIZE}"
        ;;
    vm)
        echo "VM Bytes: ${VM_BYTES}"
        ;;
esac
[[ -n "${TASKSET}" ]] && echo "Taskset: ${TASKSET}"
[[ "${VERIFY}" == "1" ]] && echo "Verify: enabled"
[[ "${AGGRESSIVE}" == "1" ]] && echo "Aggressive: enabled"
echo ""

# Build stress-ng args
STRESS_ARGS=(
    "--timeout" "${TIMEOUT}s"
    "--metrics-brief"
    "--yaml" "${YAML_FILE}"
    "--times"
)

# Add stressor-specific arguments
case "$STRESSOR" in
    cpu)
        STRESS_ARGS+=("--cpu" "${WORKERS}")
        [[ -n "${CPU_METHOD}" ]] && STRESS_ARGS+=("--cpu-method" "${CPU_METHOD}")
        ;;
    cache)
        STRESS_ARGS+=("--cache" "${WORKERS}")
        ;;
    matrix)
        STRESS_ARGS+=("--matrix" "${WORKERS}")
        STRESS_ARGS+=("--matrix-size" "${MATRIX_SIZE}")
        ;;
    vecmath)
        STRESS_ARGS+=("--vecmath" "${WORKERS}")
        ;;
    vecwide)
        STRESS_ARGS+=("--vecwide" "${WORKERS}")
        ;;
    bsearch)
        STRESS_ARGS+=("--bsearch" "${WORKERS}")
        ;;
    qsort)
        STRESS_ARGS+=("--qsort" "${WORKERS}")
        ;;
    zlib)
        STRESS_ARGS+=("--zlib" "${WORKERS}")
        ;;
    stream)
        STRESS_ARGS+=("--stream" "${WORKERS}")
        ;;
    vm)
        STRESS_ARGS+=("--vm" "${WORKERS}")
        STRESS_ARGS+=("--vm-bytes" "${VM_BYTES}")
        ;;
    *)
        echo "ERROR: Unknown stressor type: ${STRESSOR}"
        echo "Valid options: cpu, cache, matrix, vecmath, vecwide, bsearch, qsort, zlib, stream, vm"
        exit 1
        ;;
esac

# Add optional flags
[[ "${VERIFY}" == "1" ]] && STRESS_ARGS+=("--verify")
[[ "${AGGRESSIVE}" == "1" ]] && STRESS_ARGS+=("--aggressive")
[[ -n "${TASKSET}" ]] && STRESS_ARGS+=("--taskset" "${TASKSET}")

# Run stress-ng benchmark
echo "====================================================================="
echo "Running stress-ng Benchmark"
echo "====================================================================="
echo "Command: stress-ng ${STRESS_ARGS[*]}"
echo ""

stress-ng "${STRESS_ARGS[@]}"
STRESS_EXIT_CODE=$?

echo ""

# Output YAML metrics for parser consumption
echo "====================================================================="
echo "YAML Metrics Output"
echo "====================================================================="
if [[ -f "${YAML_FILE}" ]]; then
    echo "BEGIN_STRESS_NG_YAML"
    cat "${YAML_FILE}"
    echo "END_STRESS_NG_YAML"
else
    echo "Warning: YAML metrics file not found at ${YAML_FILE}"
fi
echo ""

# Post-benchmark metrics
echo "====================================================================="
echo "Post-Benchmark Metrics"
echo "====================================================================="

# CPU frequency after test
echo "CPU Frequency (post-test):"
if command -v cpupower &> /dev/null; then
    cpupower frequency-info 2>/dev/null | grep -E 'current CPU' || true
else
    cat /proc/cpuinfo | grep -m1 'cpu MHz' || true
fi
echo ""

# Memory status
echo "Memory Status:"
free -h
echo ""

# Thermal status
echo "Thermal Status:"
if [[ -d /sys/class/thermal ]]; then
    for zone in /sys/class/thermal/thermal_zone*; do
        if [[ -f "${zone}/type" ]] && [[ -f "${zone}/temp" ]]; then
            ZONE_TYPE=$(cat "${zone}/type" 2>/dev/null)
            ZONE_TEMP=$(cat "${zone}/temp" 2>/dev/null)
            if [[ -n "${ZONE_TEMP}" ]]; then
                echo "  ${ZONE_TYPE}: $((ZONE_TEMP / 1000))C"
            fi
        fi
    done
else
    echo "  No thermal zones found"
fi
echo ""

echo "====================================================================="
echo "Benchmark Execution Complete"
echo "====================================================================="
echo "Exit Code: ${STRESS_EXIT_CODE}"
echo "Log File: ${LOG_FILE}"
echo ""

exit "${STRESS_EXIT_CODE}"
