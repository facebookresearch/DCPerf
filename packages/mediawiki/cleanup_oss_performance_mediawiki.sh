#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
MW_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$MW_ROOT/../..")"

# Function to detect if running in Docker container
is_docker_container() {
  # Check for .dockerenv file (most reliable method)
  if [ -f /.dockerenv ]; then
    return 0
  fi
  return 1
}

# Function to stop MariaDB in Docker container
stop_mariadb_docker() {
  echo "Stopping MariaDB in Docker container mode..."
  pkill mariadb
}

# Function to stop MariaDB on bare-metal machine
stop_mariadb_systemctl() {
  echo "Stopping MariaDB using systemctl..."
  sudo systemctl stop mariadb
}

# Stop MariaDB using appropriate method based on environment
if is_docker_container; then
  stop_mariadb_docker
else
  stop_mariadb_systemctl
fi

rm -rf "${BENCHPRESS_ROOT}/oss-performance"
rm -rf "${BENCHPRESS_ROOT}/benchmarks/oss_performance_mediawiki"
