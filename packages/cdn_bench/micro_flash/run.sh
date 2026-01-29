#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Running Flash Micro"

FLASH_MICRO_DIR="$(dirname "$(realpath "$0")")"
LOG_FILE="${FLASH_MICRO_DIR}/fio_run.log"

# Default values
FILENAME="/dev/nvme0n1"
DIR="/mnt/test"
SIZE="10G"
FILESIZE="1G"
NRFILES="4"
OFFSET="0"
OFFSET_INCREMENT="1G"
DIRECT="1"
RW="randread"
RWMIXREAD="70"
BS="4k"
BSRANGE="4k-64k"
RANDOM_DISTRIBUTION="random"
NUMJOBS="8"
THREAD="1"
IODEPTH="32"
GROUP_REPORTING="1"
RUNTIME="300"
TIME_BASED="1"
RAMP_TIME="30"
TIMEOUT="600"
LOOPS="1"
IOENGINE="libaio"
SYNC="0"
HIPRI="0"
RATE=""
RATE_IOPS=""
RATE_PROCESS="0"
CPUS_ALLOWED=""
CPUS_ALLOWED_POLICY="shared"
NUMA_CPU_NODES=""
NUMA_MEM_POLICY="default"

# Parse command line arguments (--option=value format)
for arg in "$@"; do
    case "$arg" in
        --filename=*) FILENAME="${arg#*=}" ;;
        --directory=*) DIR="${arg#*=}" ;;
        --size=*) SIZE="${arg#*=}" ;;
        --filesize=*) FILESIZE="${arg#*=}" ;;
        --nrfiles=*) NRFILES="${arg#*=}" ;;
        --offset=*) OFFSET="${arg#*=}" ;;
        --offset_increment=*) OFFSET_INCREMENT="${arg#*=}" ;;
        --direct=*) DIRECT="${arg#*=}" ;;
        --rw=*) RW="${arg#*=}" ;;
        --rwmixread=*) RWMIXREAD="${arg#*=}" ;;
        --bs=*) BS="${arg#*=}" ;;
        --bsrange=*) BSRANGE="${arg#*=}" ;;
        --random_distribution=*) RANDOM_DISTRIBUTION="${arg#*=}" ;;
        --numjobs=*) NUMJOBS="${arg#*=}" ;;
        --thread=*) THREAD="${arg#*=}" ;;
        --iodepth=*) IODEPTH="${arg#*=}" ;;
        --group_reporting=*) GROUP_REPORTING="${arg#*=}" ;;
        --runtime=*) RUNTIME="${arg#*=}" ;;
        --time_based=*) TIME_BASED="${arg#*=}" ;;
        --ramp_time=*) RAMP_TIME="${arg#*=}" ;;
        --timeout=*) TIMEOUT="${arg#*=}" ;;
        --loops=*) LOOPS="${arg#*=}" ;;
        --ioengine=*) IOENGINE="${arg#*=}" ;;
        --sync=*) SYNC="${arg#*=}" ;;
        --hipri=*) HIPRI="${arg#*=}" ;;
        --rate=*) RATE="${arg#*=}" ;;
        --rate_iops=*) RATE_IOPS="${arg#*=}" ;;
        --rate_process=*) RATE_PROCESS="${arg#*=}" ;;
        --cpus_allowed=*) CPUS_ALLOWED="${arg#*=}" ;;
        --cpus_allowed_policy=*) CPUS_ALLOWED_POLICY="${arg#*=}" ;;
        --numa_cpu_nodes=*) NUMA_CPU_NODES="${arg#*=}" ;;
        --numa_mem_policy=*) NUMA_MEM_POLICY="${arg#*=}" ;;
    esac
done

# Check if fio is installed
if ! command -v fio &> /dev/null; then
    echo "ERROR: fio is not installed. Run install_flash_micro.sh first."
    exit 1
fi

# Build cli args
FIO_ARGS=(
    "--name=flash_micro"
    "--output-format=json"
)

# Determine target: use filename (block device) or directory (file-based)
if [[ "${FILENAME}" == /dev/* ]]; then
    FIO_ARGS+=("--filename=${FILENAME}")
else
    FIO_ARGS+=("--directory=${DIR}")
    FIO_ARGS+=("--filesize=${FILESIZE}")
    FIO_ARGS+=("--nrfiles=${NRFILES}")
fi

# Add core FIO parameters
FIO_ARGS+=("--size=${SIZE}")
FIO_ARGS+=("--direct=${DIRECT}")
FIO_ARGS+=("--rw=${RW}")
FIO_ARGS+=("--bs=${BS}")
FIO_ARGS+=("--numjobs=${NUMJOBS}")
FIO_ARGS+=("--iodepth=${IODEPTH}")
FIO_ARGS+=("--ioengine=${IOENGINE}")
FIO_ARGS+=("--runtime=${RUNTIME}")
FIO_ARGS+=("--ramp_time=${RAMP_TIME}")

# Add optional parameters if set
[[ -n "${OFFSET}" && "${OFFSET}" != "0" ]] && FIO_ARGS+=("--offset=${OFFSET}")
[[ -n "${OFFSET_INCREMENT}" ]] && FIO_ARGS+=("--offset_increment=${OFFSET_INCREMENT}")
[[ "${RW}" == *"rw"* ]] && FIO_ARGS+=("--rwmixread=${RWMIXREAD}")
[[ -n "${BSRANGE}" ]] && FIO_ARGS+=("--bsrange=${BSRANGE}")
[[ -n "${RANDOM_DISTRIBUTION}" && "${RANDOM_DISTRIBUTION}" != "random" ]] && FIO_ARGS+=("--random_distribution=${RANDOM_DISTRIBUTION}")
[[ "${THREAD}" == "1" ]] && FIO_ARGS+=("--thread")
[[ "${GROUP_REPORTING}" == "1" ]] && FIO_ARGS+=("--group_reporting")
[[ "${TIME_BASED}" == "1" ]] && FIO_ARGS+=("--time_based")
[[ -n "${TIMEOUT}" ]] && FIO_ARGS+=("--timeout=${TIMEOUT}")
[[ "${LOOPS}" != "1" ]] && FIO_ARGS+=("--loops=${LOOPS}")
[[ "${SYNC}" == "1" ]] && FIO_ARGS+=("--sync=1")
[[ "${HIPRI}" == "1" ]] && FIO_ARGS+=("--hipri=1")
[[ -n "${RATE}" ]] && FIO_ARGS+=("--rate=${RATE}")
[[ -n "${RATE_IOPS}" ]] && FIO_ARGS+=("--rate_iops=${RATE_IOPS}")
[[ "${RATE_PROCESS}" == "1" ]] && FIO_ARGS+=("--rate_process=linear")
[[ -n "${CPUS_ALLOWED}" ]] && FIO_ARGS+=("--cpus_allowed=${CPUS_ALLOWED}")
[[ -n "${CPUS_ALLOWED_POLICY}" ]] && FIO_ARGS+=("--cpus_allowed_policy=${CPUS_ALLOWED_POLICY}")
[[ -n "${NUMA_CPU_NODES}" ]] && FIO_ARGS+=("--numa_cpu_nodes=${NUMA_CPU_NODES}")
[[ "${NUMA_MEM_POLICY}" != "default" ]] && FIO_ARGS+=("--numa_mem_policy=${NUMA_MEM_POLICY}")

# Collect system metadata before benchmark
echo "====================================================================="
echo "Flash Micro Benchmark (FIO)"
echo "====================================================================="
echo "Script: $0"
echo "Execution Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "FIO Version: $(fio --version)"
echo ""

echo "====================================================================="
echo "Configuration"
echo "====================================================================="
echo "Target:"
if [[ "${FILENAME}" == /dev/* ]]; then
    echo "  - Device: ${FILENAME}"
else
    echo "  - Directory: ${DIR}"
    echo "  - File Size: ${FILESIZE}"
    echo "  - Number of Files: ${NRFILES}"
fi
echo ""
echo "I/O Pattern:"
echo "  - Access Pattern: ${RW}"
[[ "${RW}" == *"rw"* ]] && echo "  - Read/Write Mix: ${RWMIXREAD}% read"
echo "  - Block Size: ${BS}"
echo "  - Direct I/O: ${DIRECT}"
echo ""
echo "Concurrency:"
echo "  - Number of Jobs: ${NUMJOBS}"
echo "  - I/O Depth: ${IODEPTH}"
echo "  - I/O Engine: ${IOENGINE}"
echo ""
echo "Timing:"
echo "  - Runtime: ${RUNTIME}s"
echo "  - Ramp Time: ${RAMP_TIME}s"
echo ""

# Storage device information
echo "====================================================================="
echo "Storage Device Information"
echo "====================================================================="
if [[ "${FILENAME}" == /dev/nvme* ]]; then
    NVME_DEV=$(echo "${FILENAME}" | grep -oP 'nvme\d+')
    if [[ -n "$NVME_DEV" ]]; then
        echo "NVMe Device: $NVME_DEV"
        nvme list 2>/dev/null | grep -E "^/dev/$NVME_DEV|^Node" || echo "nvme-cli not available"
        echo ""
        echo "NVMe Smart Log:"
        nvme smart-log "/dev/$NVME_DEV" 2>/dev/null | head -20 || echo "Unable to get smart log"
    fi
elif [[ "${FILENAME}" == /dev/* ]]; then
    echo "Block Device: ${FILENAME}"
    lsblk "${FILENAME}" 2>/dev/null || echo "Unable to get device info"
fi
echo ""

# Mount point information
echo "====================================================================="
echo "Mount Point Information"
echo "====================================================================="
if [[ "${FILENAME}" != /dev/* ]]; then
    df -h "${DIR}" 2>/dev/null || echo "Directory not mounted"
fi
mount | grep -E "nvme|sd[a-z]" | head -10 || echo "No relevant mounts found"
echo ""

# Run FIO benchmark
echo "====================================================================="
echo "Running FIO Benchmark"
echo "====================================================================="
echo "Command: fio ${FIO_ARGS[*]}"
echo ""

# Execute FIO and capture output
fio "${FIO_ARGS[@]}" 2>&1 | tee "$LOG_FILE"
FIO_EXIT_CODE=${PIPESTATUS[0]}

echo ""

# Post-benchmark metrics
echo "====================================================================="
echo "Post-Benchmark Metrics"
echo "====================================================================="

# I/O scheduler information
echo "I/O Scheduler:"
if [[ "${FILENAME}" == /dev/nvme* ]]; then
    NVME_DEV=$(echo "${FILENAME}" | grep -oP 'nvme\d+n\d+' || echo "${FILENAME##*/}")
    cat "/sys/block/$NVME_DEV/queue/scheduler" 2>/dev/null || echo "Unable to get scheduler"
fi
echo ""

# Disk stats after benchmark
echo "Disk Statistics:"
if [[ "${FILENAME}" == /dev/* ]]; then
    DEV_NAME="${FILENAME##*/}"
    grep "$DEV_NAME" /proc/diskstats || echo "Unable to get disk stats"
fi
echo ""

# Memory pressure during test
echo "Memory Status:"
free -h
echo ""

echo "====================================================================="
echo "Benchmark Execution Complete"
echo "====================================================================="
echo "Exit Code: $FIO_EXIT_CODE"
echo "Log File: $LOG_FILE"
echo ""

exit $FIO_EXIT_CODE
