#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

NIC_MICRO_DIR="$(dirname "$(readlink -f "$0")")"

echo "Cleaning up nic_micro directory..."

cd "$NIC_MICRO_DIR" || exit 1

# Kill any running iperf3 processes started by this benchmark
echo "Checking for running iperf3 processes..."
if pgrep -x iperf3 > /dev/null 2>&1; then
    echo "Stopping running iperf3 processes..."
    pkill -x iperf3 2>/dev/null || true
    sleep 1
    # Force kill if still running
    if pgrep -x iperf3 > /dev/null 2>&1; then
        echo "Force killing iperf3 processes..."
        pkill -9 -x iperf3 2>/dev/null || true
    fi
    echo "iperf3 processes stopped."
else
    echo "No running iperf3 processes found."
fi

# Remove NIC info log file
if [ -f nic_info.log ]; then
    echo "Removing nic_info.log..."
    rm -f nic_info.log
fi


# Clean up any iperf3 server PID files if they exist
if [ -f iperf3.pid ]; then
    echo "Removing iperf3.pid..."
    rm -f iperf3.pid
fi

echo "Cleanup complete!"
