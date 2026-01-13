#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Install prerequisite packages
##########################################
echo "Installing prerequisite packages..."
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y gcc make linux-tools-common linux-tools-generic linux-tools-"$(uname -r)" wget
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y gcc make perf wget
fi

echo "Installation complete!"
