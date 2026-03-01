#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail


CDN_PACKAGE_DIR="$(dirname "$(readlink -f "$0")")"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

# Wrk
WRK_GIT_REPO='https://github.com/wg/wrk/'
WRK_GIT_RELEASE_TAG='4.2.0'

# Cachelib
# CACHELIB_GIT_REPO='https://github.com/facebook/CacheLib'
# CACHELIB_GIT_RELEASE_TAG='main'

##########################################
# Install prerequisite packages
##########################################
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y  autoconf automake libevent-dev zlib1g zlib1g-dev perl
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y autoconf automake libevent-devel zlib-devel perl
fi

###########################################
# Install Nginx
###########################################
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y nginx
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y nginx
fi

############################################
# Install Wrk
############################################
if ! [ -x "$(command -v wrk)" ]; then
  # shellcheck disable=SC2046
  git clone "$WRK_GIT_REPO"
  cd wrk || exit 1
  # shellcheck disable=SC2046
  git checkout "$WRK_GIT_RELEASE_TAG"
  make -j8
  # copy to local binaries
  cp ./wrk /usr/local/bin/wrk
  cd ..
fi


# ############################################
# # Install Cachebench
# ############################################
# if ! [ -d ./CacheLib ]; then
#   git clone "$CACHELIB_GIT_REPO"
# fi
# cd CacheLib
# # git checkout "$CACHELIB_GIT_RELEASE_TAG"

# # install deps for server build
# ./build/fbcode_builder/getdeps.py install-system-deps cachelib
# ./build/fbcode_builder/getdeps.py build --allow-system-packages cachelib
# ./build/fbcode_builder/getdeps.py build --allow-system-packages proxygen

# # build cachelib server
# cd ./examples/proxygen_cache
# ./build.sh
# cp ./build/proxygen_cache /usr/local/bin/proxygen_cache
