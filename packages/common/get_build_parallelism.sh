#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# =============================================================================
# get_build_parallelism.sh
#
# Calculates the optimal number of parallel build jobs based on:
# 1. Number of logical CPU cores available (considering cgroup limits)
# 2. Available memory in GB divided by 2.0 (considering cgroup limits)
#
# The final value is the minimum of these two constraints to prevent OOM
# during compilation-heavy builds.
#
# Usage:
#   source /path/to/get_build_parallelism.sh
#   NUM_BUILD_JOBS=$(get_build_parallelism)
#
# Or override with environment variable:
#   NUM_BUILD_JOBS=8 ./install_script.sh
# =============================================================================

# Get the number of CPU cores available, considering cgroup limits
get_cpu_cores() {
    local cores

    # Try cgroup v2 first (cpu.max)
    if [ -f /sys/fs/cgroup/cpu.max ]; then
        local cpu_max
        cpu_max=$(cat /sys/fs/cgroup/cpu.max 2>/dev/null)
        local quota period
        quota=$(echo "$cpu_max" | awk '{print $1}')
        period=$(echo "$cpu_max" | awk '{print $2}')

        if [ "$quota" != "max" ] && [ -n "$quota" ] && [ -n "$period" ] && [ "$period" -gt 0 ]; then
            # cgroup v2: cores = quota / period
            cores=$(awk "BEGIN {printf \"%.0f\", $quota / $period}")
            if [ "$cores" -gt 0 ]; then
                echo "$cores"
                return
            fi
        fi
    fi

    # Try cgroup v1 (cpu.cfs_quota_us / cpu.cfs_period_us)
    if [ -f /sys/fs/cgroup/cpu/cpu.cfs_quota_us ] && [ -f /sys/fs/cgroup/cpu/cpu.cfs_period_us ]; then
        local quota period
        quota=$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null)
        period=$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null)

        if [ "$quota" -gt 0 ] && [ "$period" -gt 0 ]; then
            # cgroup v1: cores = quota / period
            cores=$(awk "BEGIN {printf \"%.0f\", $quota / $period}")
            if [ "$cores" -gt 0 ]; then
                echo "$cores"
                return
            fi
        fi
    fi

    # Fallback to nproc (physical/logical cores)
    nproc
}

# Get the available memory in GB, considering cgroup limits
get_memory_gb() {
    local mem_bytes=""

    # Try cgroup v2 first (memory.max)
    if [ -f /sys/fs/cgroup/memory.max ]; then
        local cgroup_mem
        cgroup_mem=$(cat /sys/fs/cgroup/memory.max 2>/dev/null || echo "")

        if [ -n "$cgroup_mem" ] && [ "$cgroup_mem" != "max" ]; then
            mem_bytes="$cgroup_mem"
        fi
    fi

    # Try cgroup v1 (memory.limit_in_bytes)
    if [ -z "$mem_bytes" ] && [ -f /sys/fs/cgroup/memory/memory.limit_in_bytes ]; then
        local cgroup_mem
        cgroup_mem=$(cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null || echo "")

        # Check if it's not the max value (9223372036854771712 or similar)
        if [ -n "$cgroup_mem" ] && [ "$cgroup_mem" -lt 9000000000000000000 ] 2>/dev/null; then
            mem_bytes="$cgroup_mem"
        fi
    fi

    # Fallback to system memory from /proc/meminfo
    if [ -z "$mem_bytes" ]; then
        local mem_kb
        mem_kb=$(grep MemTotal /proc/meminfo 2>/dev/null | awk '{print $2}')
        if [ -n "$mem_kb" ]; then
            mem_bytes=$((mem_kb * 1024))
        fi
    fi

    # Convert to GB (integer)
    if [ -n "$mem_bytes" ]; then
        awk "BEGIN {printf \"%.0f\", $mem_bytes / 1024 / 1024 / 1024}"
    else
        # Default to 4GB if we can't determine memory
        echo "4"
    fi
}

# Calculate optimal build parallelism
get_build_parallelism() {
    local cpu_cores mem_gb mem_based_jobs

    cpu_cores=$(get_cpu_cores)
    mem_gb=$(get_memory_gb)

    # Calculate memory-based job limit: memory_gb / 2.0
    # This assumes ~2GB per compilation job for heavy C++ builds
    mem_based_jobs=$(awk "BEGIN {printf \"%.0f\", $mem_gb / 2.0}")

    # Ensure at least 1 job
    if [ "$mem_based_jobs" -lt 1 ]; then
        mem_based_jobs=1
    fi

    # Return the minimum of CPU cores and memory-based limit
    if [ "$cpu_cores" -lt "$mem_based_jobs" ]; then
        echo "$cpu_cores"
    else
        echo "$mem_based_jobs"
    fi
}

# Print diagnostic information (useful for debugging)
print_build_parallelism_info() {
    local cpu_cores mem_gb mem_based_jobs final_jobs

    cpu_cores=$(get_cpu_cores)
    mem_gb=$(get_memory_gb)
    mem_based_jobs=$(awk "BEGIN {printf \"%.0f\", $mem_gb / 2.0}")

    if [ "$mem_based_jobs" -lt 1 ]; then
        mem_based_jobs=1
    fi

    if [ "$cpu_cores" -lt "$mem_based_jobs" ]; then
        final_jobs="$cpu_cores"
    else
        final_jobs="$mem_based_jobs"
    fi

    echo "====================================================================="
    echo "Build Parallelism Configuration"
    echo "====================================================================="
    echo "CPU cores available (cgroup-aware):     $cpu_cores"
    echo "Memory available (cgroup-aware):        ${mem_gb} GB"
    echo "Memory-based job limit (mem_gb / 2.0):  $mem_based_jobs"
    echo "Final parallel jobs (minimum):          $final_jobs"
    echo "====================================================================="
}

# If script is run directly (not sourced), print the value
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    if [ "$1" == "--info" ] || [ "$1" == "-i" ]; then
        print_build_parallelism_info
    else
        get_build_parallelism
    fi
fi
