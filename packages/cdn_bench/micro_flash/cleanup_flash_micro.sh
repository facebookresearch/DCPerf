#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Cleanup Flash Micro"


FLASH_MICRO_DIR="$(dirname "$(readlink -f "$0")")"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Install prerequisite packages
##########################################
echo "Installing prerequisite packages..."
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt remove -y fio fio-engine-libaio
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf remove -y fio fio-engine-libaio
fi

cd "$FLASH_MICRO_DIR" || exit 1

# Remove log files
if [ -f fio_run.log ]; then
  echo "Removing fio_run.log..."
  rm -f fio_run.log
fi

# Remove any other log files
echo "Removing any other log files..."
rm -f ./*.log

echo "Cleanup complete!"
