#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

PKG_ROOT="$(dirname "$(readlink -f "$0")")"

rm -rf "${PKG_ROOT:?}/bin"
rm -rf "${PKG_ROOT:?}/datasets"
rm -rf "${PKG_ROOT:?}/build"
rm -rf "${PKG_ROOT:?}/work"
