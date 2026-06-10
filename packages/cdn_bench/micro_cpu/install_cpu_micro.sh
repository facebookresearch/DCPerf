#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
echo "Installing CPU Micro"

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
CHECKSUM_FILE="$SCRIPT_DIR/.package_state.json"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

##########################################
# Utility functions
##########################################

# Get version of a specific package (ubuntu)
get_ubuntu_version() {
  local package="$1"
  dpkg-query -W -f='${Version}' "$package" 2>/dev/null || echo ""
}

# Get version of a specific package (centos)
get_centos_version() {
  local package="$1"
  local version
  version=$(rpm -q --qf '%{VERSION}-%{RELEASE}' "$package" 2>&1)
  # Only return if command succeeded (exit code 0)
  if [[ $? -eq 0 && -n "$version" && ! "$version" =~ "is not installed" ]]; then
    echo "$version"
  else
    echo ""
  fi
}

# Get the latest available version from repo for ubuntu
get_ubuntu_latest_version() {
  local package="$1"
  apt-cache policy "$package" 2>/dev/null | grep "Candidate:" | awk '{print $2}' || echo ""
}

# Get the latest available version from repo for centos
get_centos_latest_version() {
  local package="$1"
  local output
  output=$(dnf repoquery --latest-limit=1 "$package" 2>/dev/null)
  # Extract version-release from "package-epoch:version-release.arch" format
  if [[ -n "$output" ]]; then
    echo "$output" | sed -E 's/^.*:(.+)\.x86_64$/\1/'
  else
    echo ""
  fi
}

# Check if package needs installation (not installed or version differs)
ubuntu_needs_install() {
  local package="$1"
  local current_version
  local latest_version

  current_version=$(get_ubuntu_version "$package")
  latest_version=$(get_ubuntu_latest_version "$package")

  if [[ -z "$current_version" ]]; then
    echo "new"
  elif [[ "$current_version" != "$latest_version" && -n "$latest_version" ]]; then
    echo "upgrade"
  else
    echo "skip"
  fi
}

# Check if package needs installation (not installed or version differs)
centos_needs_install() {
  local package="$1"
  local current_version
  local latest_version

  current_version=$(get_centos_version "$package")
  latest_version=$(get_centos_latest_version "$package")

  if [[ -z "$current_version" ]]; then
    echo "new"
  elif [[ "$current_version" != "$latest_version" && -n "$latest_version" ]]; then
    echo "upgrade"
  else
    echo "skip"
  fi
}

##########################################
# Capture pre-install state of target packages
##########################################
echo "Capturing current package state..."

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  TARGET_PACKAGES=("stress-ng" "numactl" "linux-tools-common")
  declare -A PRE_INSTALL_STATE
  for pkg in "${TARGET_PACKAGES[@]}"; do
    PRE_INSTALL_STATE["$pkg"]=$(get_ubuntu_version "$pkg")
  done
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  TARGET_PACKAGES=("stress-ng" "numactl" "kernel-tools")
  declare -A PRE_INSTALL_STATE
  for pkg in "${TARGET_PACKAGES[@]}"; do
    PRE_INSTALL_STATE["$pkg"]=$(get_centos_version "$pkg")
  done
fi

##########################################
# Install prerequisite packages
##########################################
echo "Installing prerequisite packages..."
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  PACKAGES_TO_INSTALL=()
  PACKAGES_TO_UPGRADE=()

  for pkg in "${TARGET_PACKAGES[@]}"; do
    status=$(ubuntu_needs_install "$pkg")
    if [[ "$status" == "new" ]]; then
      PACKAGES_TO_INSTALL+=("$pkg")
    elif [[ "$status" == "upgrade" ]]; then
      PACKAGES_TO_UPGRADE+=("$pkg")
    else
      echo "  $pkg already at latest version, skipping"
    fi
  done

  if [[ ${#PACKAGES_TO_INSTALL[@]} -gt 0 ]]; then
    apt install -y "${PACKAGES_TO_INSTALL[@]}"
  fi
  if [[ ${#PACKAGES_TO_UPGRADE[@]} -gt 0 ]]; then
    apt install -y --only-upgrade "${PACKAGES_TO_UPGRADE[@]}"
  fi
  if [[ ${#PACKAGES_TO_INSTALL[@]} -eq 0 && ${#PACKAGES_TO_UPGRADE[@]} -eq 0 ]]; then
    echo "All packages already at latest version"
  fi

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  PACKAGES_TO_INSTALL=()
  PACKAGES_TO_UPGRADE=()

  for pkg in "${TARGET_PACKAGES[@]}"; do
    status=$(centos_needs_install "$pkg")
    if [[ "$status" == "new" ]]; then
      PACKAGES_TO_INSTALL+=("$pkg")
    elif [[ "$status" == "upgrade" ]]; then
      PACKAGES_TO_UPGRADE+=("$pkg")
    else
      echo "  $pkg already at latest version, skipping"
    fi
  done

  if [[ ${#PACKAGES_TO_INSTALL[@]} -gt 0 ]]; then
    dnf install -y "${PACKAGES_TO_INSTALL[@]}"
  fi
  if [[ ${#PACKAGES_TO_UPGRADE[@]} -gt 0 ]]; then
    dnf upgrade -y "${PACKAGES_TO_UPGRADE[@]}"
  fi
  if [[ ${#PACKAGES_TO_INSTALL[@]} -eq 0 && ${#PACKAGES_TO_UPGRADE[@]} -eq 0 ]]; then
    echo "All packages already at latest version"
  fi
fi

# Capture post-install state and compare
echo "Capturing post-install package state..."

# Create checksum file with target packages and their versions
{
  printf '{\n'
  printf '  "distro": "%s",\n' "$LINUX_DIST_ID"
  printf '  "packages": {\n'

  first_package=true

  for pkg in "${TARGET_PACKAGES[@]}"; do
    # Get post-install version
    if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
      post_version=$(get_ubuntu_version "$pkg")
    else
      post_version=$(get_centos_version "$pkg")
    fi

    # Get pre-install version from our state tracking
    prior_version="${PRE_INSTALL_STATE[$pkg]}"

    # Determine if this is new (wasn't installed before)
    is_new=false
    if [[ -z "$prior_version" ]]; then
      is_new=true
    fi

    if [[ "$first_package" == true ]]; then
      first_package=false
    else
      printf ',\n'
    fi

    if [[ "$is_new" == true ]]; then
      printf '    "%s": {"version": "%s", "prior_version": null, "is_new": true}' "$pkg" "$post_version"
    else
      printf '    "%s": {"version": "%s", "prior_version": "%s", "is_new": false}' "$pkg" "$post_version" "$prior_version"
    fi
  done

  printf '\n'
  printf '  }\n'
  printf '}\n'
} > "$CHECKSUM_FILE"

echo "Installation complete!"
echo "Package state saved to $CHECKSUM_FILE"
