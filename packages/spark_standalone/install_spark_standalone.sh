#!/bin/bash
set -Eeuo pipefail

SPARK_PKG_ROOT="$(dirname "$(readlink -f "$0")")"

# benchmark binaries that we install here live in benchmarks/
TEMPLATES_DIR="${SPARK_PKG_ROOT}/templates"

# Install system dependencies
dnf install -y java-1.8.0-openjdk
dnf install -y git-lfs

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
