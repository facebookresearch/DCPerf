#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Cleanup CPU Micro"


CPU_MICRO_DIR="$(dirname "$(readlink -f "$0")")"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Remove prerequisite packages
##########################################
echo "Removing prerequisite packages..."
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt remove -y sysbench
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf remove -y sysbench
fi

cd "$CPU_MICRO_DIR" || exit 1

# Remove log files
if [ -f cpu_run.log ]; then
  echo "Removing cpu_run.log..."
  rm -f cpu_run.log
fi

# Remove any other log files
echo "Removing any other log files..."
rm -f ./*.log

echo "Cleanup complete!"
