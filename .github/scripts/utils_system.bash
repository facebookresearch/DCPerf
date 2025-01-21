#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"

################################################################################
# System Functions
################################################################################

install_system_packages () {
  if [ $# -le 0 ]; then
    echo "Usage: ${FUNCNAME[0]} PACKAGE_NAME ... "
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} miopen-hip miopen-hip-dev"
    return 1
  fi

  test_network_connection || return 1

  if which sudo; then
    local update_cmd=(sudo)
    local install_cmd=(sudo)
  else
    local update_cmd=()
    local install_cmd=()
  fi

  if which apt-get; then
    update_cmd+=(apt update -y)
    install_cmd+=(apt install -y "$@")
  elif which yum; then
    update_cmd+=(yum update -y)
    install_cmd+=(yum install -y "$@")
  else
    echo "[INSTALL] Could not find a system package installer to install packages!"
    return 1
  fi

  echo "[INSTALL] Updating system repositories ..."
  # shellcheck disable=SC2068
  (exec_with_retries 3 ${update_cmd[@]}) || return 1

  # shellcheck disable=SC2145
  echo "[INSTALL] Installing system package(s): $@ ..."
  # shellcheck disable=SC2068
  (exec_with_retries 3 ${install_cmd[@]}) || return 1
}

free_disk_space () {
  echo "################################################################################"
  echo "# Free Disk Space"
  echo "#"
  echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
  echo "################################################################################"
  echo ""

  sudo rm -rf \
    /usr/local/android \
    /usr/share/dotnet \
    /usr/local/share/boost \
    /opt/ghc \
    /usr/local/share/chrom* \
    /usr/share/swift \
    /usr/local/julia* \
    /usr/local/lib/android

  echo "[CLEANUP] Freed up some disk space"
}

free_disk_space_on_host () {
  echo "################################################################################"
  echo "# Free Disk Space On CI Host"
  echo "################################################################################"

  # NOTE: This is meant to be run from ** inside ** containers hosted on
  # non-PyTorch-infra GitHub runners, where the hosts might be close to full
  # disk from serving many CI jobs.  When the container is set up properly, we
  # can escape the container using nsenter to run commands on the host.
  #
  # On average, we see roughly 3GB of disk freed when running this cleanup,
  # which appears to be sufficient to avoid the somewhat-frequent out-of-disk
  # errors that we were previously running into.
  #
  # Frees up disk space on the ubuntu-latest host machine based on recommendations:
  # https://github.com/orgs/community/discussions/25678
  # https://github.com/apache/flink/blob/02d30ace69dc18555a5085eccf70ee884e73a16e/tools/azure-pipelines/free_disk_space.sh
  #
  # Escape the docker container to run the free disk operation on the host:
  # https://stackoverflow.com/questions/66160057/how-to-run-a-command-in-host-before-entering-docker-container-in-github-ci
  # https://stackoverflow.com/questions/32163955/how-to-run-shell-script-on-host-from-docker-container/63140387#63140387

  nsenter -t 1 -m -u -n -i bash -c "
    echo 'Listing 100 largest packages';
    dpkg-query -Wf '\${Installed-Size}\t\${Package}\n' | sort -n | tail -n 100;
    df -h;

    echo 'Removing large packages';
    sudo apt-get remove -y '^ghc-8.*';
    sudo apt-get remove -y '^dotnet-.*';
    sudo apt-get remove -y '^llvm-.*';
    sudo apt-get remove -y 'php.*';
    sudo apt-get remove -y azure-cli google-cloud-sdk hhvm google-chrome-stable firefox powershell mono-devel;
    sudo apt-get autoremove -y;
    sudo apt-get clean;
    df -h;

    echo 'Removing large directories';
    rm -rf /usr/local/android;
    rm -rf /usr/share/dotnet;
    rm -rf /usr/local/share/boost;
    rm -rf /opt/ghc;
    rm -rf /usr/local/share/chrom*;
    rm -rf /usr/share/swift;
    rm -rf /usr/local/julia*;
    rm -rf /usr/local/lib/android;
    rm -rf /opt/hostedtoolcache;
    df -h;
  "
}


################################################################################
# Info Functions
################################################################################

__print_system_info_linux () {
  echo "################################################################################"
  echo "[INFO] Print ldd version ..."
  print_exec ldd --version

  echo "################################################################################"
  echo "[INFO] Print CPU info ..."
  print_exec nproc
  print_exec lscpu
  print_exec cat /proc/cpuinfo


  if [[ "${BUILD_FROM_NOVA}" != '1' ]]; then
    echo "################################################################################"
    echo "[INFO] Print PCI info ..."
    print_exec lspci -v
  fi

  echo "################################################################################"
  echo "[INFO] Print Linux distribution info ..."
  print_exec uname -a
  print_exec uname -m
  print_exec cat /proc/version
  print_exec cat /etc/os-release
}

__print_system_info_macos () {
  echo "################################################################################"
  echo "[INFO] Print CPU info ..."
  sysctl -a | grep machdep.cpu

  echo "################################################################################"
  echo "[INFO] Print MacOS version info ..."
  print_exec uname -a
  print_exec sw_vers
}

print_system_info () {
  echo "################################################################################"
  echo "# Print System Info"
  echo "#"
  echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
  echo "################################################################################"
  echo ""

  echo "################################################################################"
  echo "[INFO] Printing environment variables ..."
  print_exec printenv

  if [[ $OSTYPE == 'darwin'* ]]; then
    __print_system_info_macos
  else
    __print_system_info_linux
  fi
}
