#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
MW_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$MW_ROOT/../..")"

# Detects any container, not just Docker -- the name is kept for the callers.
# Tupperware runs LXC, which has no /.dockerenv. Keep this in sync with the
# copies in run.sh and install_oss_performance_mediawiki.sh.
is_docker_container() {
  # Docker
  if [ -f /.dockerenv ]; then
    return 0
  fi
  # Podman / other OCI runtimes
  if [ -f /run/.containerenv ]; then
    return 0
  fi
  # Tupperware sets both of these in the task environment (container=lxc)
  if [ -n "${TUPD_CONTAINER:-}" ] || [ -n "${container:-}" ]; then
    return 0
  fi
  # Fallback: systemd's own detection covers lxc, docker, podman, ...
  if command -v systemd-detect-virt >/dev/null 2>&1; then
    if systemd-detect-virt --container --quiet; then
      return 0
    fi
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
