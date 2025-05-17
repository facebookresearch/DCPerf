#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"

################################################################################
# Build Tools Setup
################################################################################

install_build_tools () {
  if which dnf; then
    # Install EPEL and enable CRB
    (dnf install -y epel-release) || return 1
    (dnf install 'dnf-command(config-manager)') || return 1
    (dnf config-manager --set-enabled crb) || return 1

    # Install all other packages
    (dnf install -y \
      bc \
      binutils \
      binutils-devel \
      bison \
      bzip2-devel \
      cmake \
      double-conversion \
      double-conversion-devel \
      findutils \
      flex \
      git \
      g++ \
      gcc \
      jq \
      libaio-devel \
      libatomic \
      libdwarf \
      libdwarf-devel \
      libsodium-devel \
      libtool \
      libunwind-devel \
      libzstd-devel \
      lz4-devel \
      ninja-build \
      openssl-devel \
      patch \
      pciutils \
      perl \
      snappy-devel \
      sudo \
      tar \
      texinfo \
      wget \
      which \
      xz-devel \
      zlib-devel) || return 1
  fi
}

################################################################################
# Python Tools Setup
################################################################################

install_python_tools () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Install Build Tools"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  test_network_connection || return 1

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")
  echo "[INSTALL] Installing build tools ..."

  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    click \
    numpy=1.* \
    pandas \
    pyyaml \
    tabulate) || return 1

  # Check Python packages are importable
  local import_tests=( click numpy pandas tabulate )
  for p in "${import_tests[@]}"; do
    (test_python_import_package "${env_name}" "${p}") || return 1
  done

  echo "[INSTALL] Successfully installed all the build tools"
}
