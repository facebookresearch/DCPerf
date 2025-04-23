#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

PKG_SYSCALL_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.

make clean -C "$PKG_SYSCALL_ROOT"
