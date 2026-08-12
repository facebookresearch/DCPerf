#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

SPARK_PKG_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${SPARK_PKG_ROOT}/../..")"
BENCHMARKS_ROOT="${BENCHPRESS_ROOT}/benchmarks"
OUT="${BENCHMARKS_ROOT}/spark_standalone"

# benchmark binaries that we install here live in benchmarks/
TEMPLATES_DIR="${SPARK_PKG_ROOT}/templates"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"

# CentOS 9 runs Spark 2.4.5 on Java 8. CentOS 10 dropped Java 8 (and Spark 2.4
# can't run on Java 17+), so use Spark 4.0.3 on Java 21 there.
if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -ge 10 ]; then
  SPARK_PKG="spark-4.0.3-bin-hadoop3"
  SPARK_URL="https://archive.apache.org/dist/spark/spark-4.0.3/${SPARK_PKG}.tgz"
else
  SPARK_PKG="spark-2.4.5-bin-hadoop2.7"
  SPARK_URL="https://archive.apache.org/dist/spark/spark-2.4.5/${SPARK_PKG}.tgz"
fi

# Install system dependencies
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y openjdk-8-jdk fio
  apt install -y git-lfs
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  if [ "$VERSION_ID" -ge 10 ]; then
    dnf install -y java-21-openjdk java-21-openjdk-devel fio
  else
    dnf install -y java-1.8.0-openjdk fio
  fi
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
if [ ! -f "${SPARK_PKG}.tgz" ]; then
  wget "${SPARK_URL}"
fi
tar xzf "${SPARK_PKG}.tgz"
# Stable, version-agnostic path so the install marker matches on any Spark version.
ln -sfn "${SPARK_PKG}" spark
popd || exit 1

# create sub directories
mkdir -p "${OUT}/work"
mkdir -p "${OUT}/dataset"

echo "SPARK_Standalone installed into ${OUT}"
