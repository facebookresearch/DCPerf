#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Installing Flash Micro"

LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Install prerequisite packages
##########################################
echo "Installing prerequisite packages..."

# Install fio first
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y fio
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y fio
fi

# Check if fio has all required engines: libaio, io_uring, sync, psync, mmap
echo "Checking fio engines..."
ENGHELP_OUTPUT=$(fio --enghelp 2>/dev/null)
if echo "$ENGHELP_OUTPUT" | grep -q "libaio" && \
   echo "$ENGHELP_OUTPUT" | grep -q "io_uring" && \
   echo "$ENGHELP_OUTPUT" | grep -q "sync" && \
   echo "$ENGHELP_OUTPUT" | grep -q "psync" && \
   echo "$ENGHELP_OUTPUT" | grep -q "mmap"; then
  echo "All required fio engines found"
else
  echo "Missing required fio engines, attempting to install fio-engine-libaio..."
  if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
    if apt install -y fio-engine-libaio 2>&1 | grep -q "Unable\|E: "; then
      echo "fio-engine-libaio installation failed, installing libaio-dev..."
      apt install -y libaio-dev
    fi
  elif [ "$LINUX_DIST_ID" = "centos" ]; then
    if dnf install -y fio-engine-libaio 2>&1 | grep -q "Unable\|Error"; then
      echo "fio-engine-libaio installation failed, installing libaio-dev..."
      dnf install -y libaio-dev
    fi
  fi
fi

echo "Installation complete!"
