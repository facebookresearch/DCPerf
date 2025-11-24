#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

MEM_MICRO_DIR="$(dirname "$(readlink -f "$0")")"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

# STREAM benchmark source URL
STREAM_URL='https://www.cs.virginia.edu/stream/FTP/Code/stream.c'

##########################################
# Install prerequisite packages
##########################################
echo "Installing prerequisite packages..."
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y gcc make linux-tools-common linux-tools-generic linux-tools-"$(uname -r)" wget
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y gcc make perf wget
fi

###########################################
# Download STREAM benchmark source
###########################################
echo "Downloading STREAM benchmark source code..."
cd "$MEM_MICRO_DIR" || exit 1

if [ ! -f stream.c ]; then
  wget -O stream.c "$STREAM_URL"
  if [ ! -f stream.c ]; then
    echo "ERROR: Failed to download stream.c"
    exit 1
  fi
  echo "Successfully downloaded stream.c"
else
  echo "stream.c already exists, skipping download"
fi

echo "Installation complete!"
