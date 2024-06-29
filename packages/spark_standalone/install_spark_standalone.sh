#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

SPARK_PKG_ROOT="$(dirname "$(readlink -f "$0")")"

# benchmark binaries that we install here live in benchmarks/
TEMPLATES_DIR="${SPARK_PKG_ROOT}/templates"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

# Install system dependencies
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y openjdk-8-jdk
  apt install -y git-lfs
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y java-1.8.0-openjdk
  dnf install -y git-lfs
fi

# copy over directory
if [ ! -d "${OUT}/scripts" ]; then
  cp -r "${TEMPLATES_DIR}/proj_root/scripts" "${OUT}/"
fi
if [ ! -d "${OUT}/settings" ]; then
  cp -r "${TEMPLATES_DIR}/proj_root/settings" "${OUT}/"
fi

# download spark
pushd "${OUT}" || exit 1
if [ ! -f spark-2.4.5-bin-hadoop2.7.tgz ]; then
  wget https://archive.apache.org/dist/spark/spark-2.4.5/spark-2.4.5-bin-hadoop2.7.tgz
fi
tar xzf spark-2.4.5-bin-hadoop2.7.tgz
popd || exit 1

# create sub directories
mkdir -p "${OUT}/work"
mkdir -p "${OUT}/dataset"

echo "SPARK_Standalone installed into ./benchmarks/spark_standalone"
