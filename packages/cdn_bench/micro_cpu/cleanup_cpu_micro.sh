#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Cleanup CPU Micro"

CPU_MICRO_DIR="$(dirname "$(readlink -f "$0")")"
CHECKSUM_FILE="$CPU_MICRO_DIR/.package_state.json"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Utility functions
##########################################

# Remove a package (ubuntu)
ubuntu_remove_package() {
  local package="$1"
  echo "Removing $package..."
  apt remove -y "$package" 2>/dev/null || true
}

# Remove a package (centos)
centos_remove_package() {
  local package="$1"
  echo "Removing $package..."
  dnf remove -y "$package" 2>/dev/null || true
}

# Restore a package to prior version (ubuntu)
ubuntu_restore_package() {
  local package="$1"
  local prior_version="$2"
  echo "Restoring $package to version $prior_version..."
  apt install -y "${package}=${prior_version}" 2>/dev/null || {
    echo "  Failed to restore exact version, attempting available version..."
    apt install -y "$package" || echo "  Warning: Could not restore $package"
  }
}

# Restore a package to prior version (centos)
centos_restore_package() {
  local package="$1"
  local prior_version="$2"
  echo "Restoring $package to version $prior_version..."
  dnf install -y "${package}-${prior_version}" 2>/dev/null || {
    echo "  Failed to restore exact version, attempting available version..."
    dnf install -y "$package" || echo "  Warning: Could not restore $package"
  }
}

##########################################
# Process package state from checksum file
##########################################

if [[ ! -f "$CHECKSUM_FILE" ]]; then
  echo "Warning: Checksum file not found at $CHECKSUM_FILE"
  echo "Removing only stress-ng (conservative cleanup)..."
  if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
    apt remove -y stress-ng || true
  elif [ "$LINUX_DIST_ID" = "centos" ]; then
    dnf remove -y stress-ng || true
  fi
else
  echo "Processing package state from $CHECKSUM_FILE..."

  # Parse JSON and process each package
  while IFS= read -r line; do
    # Extract package name (skip non-package lines)
    if [[ $line =~ \"([^\"]+)\":[[:space:]]*\{\"version\" ]]; then
      pkg_name="${BASH_REMATCH[1]}"
      is_new=$(echo "$line" | grep -oP '"is_new":\s*\K(true|false)' || echo "false")
      prior_version=$(echo "$line" | grep -oP '"prior_version":\s*"\K[^"]+' || echo "")

      if [[ "$is_new" == "true" ]]; then
        # Package was newly installed, remove it
        if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
          ubuntu_remove_package "$pkg_name"
        elif [ "$LINUX_DIST_ID" = "centos" ]; then
          centos_remove_package "$pkg_name"
        fi
      elif [[ -n "$prior_version" ]]; then
        # Package was changed, try to restore prior version
        if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
          ubuntu_restore_package "$pkg_name" "$prior_version"
        elif [ "$LINUX_DIST_ID" = "centos" ]; then
          centos_restore_package "$pkg_name" "$prior_version"
        fi
      fi
    fi
  done < "$CHECKSUM_FILE"

  # Remove the checksum file
  echo "Removing checksum file..."
  rm -f "$CHECKSUM_FILE"
fi

cd "$CPU_MICRO_DIR" || exit 1

# Remove log and metrics files
echo "Removing log and metrics files..."
rm -f ./*.log
rm -f ./*.yaml

echo "Cleanup complete!"
