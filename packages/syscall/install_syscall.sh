#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### SYS CONFIG AND DEPS #########################

PKG_SYSCALL_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y libgflags-dev
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y gflags-devel
fi

# Syscall system microbenchmarks
pushd "$PKG_SYSCALL_ROOT"
make
