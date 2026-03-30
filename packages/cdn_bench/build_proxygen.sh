#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Build proxygen and all dependencies from source.
# Forked from packages/django_workload/templates/build_proxygen.sh
#
# Usage: ./build_proxygen.sh -j <num_jobs> -p <install_prefix> --proxygen-dir <proxygen_src>
#
# This script must be run from within the proxygen source tree so it can
# find version-pinning files in build/deps/github_hashes/.

set -Eeuo pipefail

# Obtain the base directory this script resides in.
BASE_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
BENCHPRESS_ROOT="$(readlink -f "${BASE_DIR}/../../")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"

# Colors
COLOR_RED="\033[0;31m"
COLOR_GREEN="\033[0;32m"
COLOR_OFF="\033[0m"

function detect_platform() {
  unameOut="$(uname -s)"
  case "${unameOut}" in
      Linux*)
        PLATFORM=Linux
        # shellcheck disable=SC1091
        source "${COMMON_DIR}/os-distro.sh"
        DISTRO="$(get_os_distro_id)"
        ;;
      *)
        echo -e "${COLOR_RED}[ ERROR ] Unsupported platform: ${unameOut}. Only Linux is supported.${COLOR_OFF}"
        exit 1
  esac
  echo -e "${COLOR_GREEN}Detected platform: $PLATFORM  Distribution: $DISTRO${COLOR_OFF}"
}

function install_dependencies_linux_default() {
  apt-get install -yq \
    git cmake m4 g++ flex bison gperf wget unzip make \
    libbz2-dev \
    libc-ares-dev \
    libgflags-dev \
    libgoogle-glog-dev \
    libkrb5-dev \
    libsasl2-dev \
    libnuma-dev \
    pkg-config \
    libssl-dev \
    libcap-dev \
    libevent-dev \
    libtool \
    libjemalloc-dev \
    libsnappy-dev \
    libiberty-dev \
    liblz4-dev \
    liblzma-dev \
    zlib1g-dev \
    binutils-dev \
    libsodium-dev \
    libdouble-conversion-dev \
    libunwind-dev
}

function install_dependencies_linux_centos() {
  dnf install -y \
    git cmake m4 g++ flex bison gperf wget unzip make \
    c-ares-devel \
    gflags-devel \
    glog-devel \
    krb5-libs \
    double-conversion-devel \
    libzstd-devel \
    libsodium-devel \
    binutils-devel \
    zlib-devel \
    lz4-devel \
    snappy-devel \
    jemalloc-devel \
    cyrus-sasl-devel \
    numactl-libs \
    openssl-devel \
    libcap-devel \
    libevent-devel \
    libunwind-devel \
    libtool \
    gperf
}

function install_dependencies() {
  echo -e "${COLOR_GREEN}[ INFO ] Installing system dependencies${COLOR_OFF}"
  case "$DISTRO" in
      centos*|rhel*|fedora*) install_dependencies_linux_centos;;
      *)                     install_dependencies_linux_default;;
  esac
}

function synch_dependency_to_commit() {
  local dep_dir="$1"
  local rev_file="$2"
  if [ "$FETCH_DEPENDENCIES" = false ]; then
    return
  fi
  DEP_REV=$(sed 's/Subproject commit //' "$rev_file")
  pushd "$dep_dir"
  git fetch
  git -c advice.detachedHead=false checkout "$DEP_REV"
  popd
}

function setup_boost() {
  BOOST_DIR=$DEPS_DIR/boost
  BOOST_VERSION="1.75.0"
  BOOST_VERSION_UNDERSCORE="1_75_0"
  BOOST_ARCHIVE="boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
  BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"

  if [ -d "$BOOST_DIR" ] && [ -f "$DEPS_DIR/lib/libboost_context.a" ]; then
    echo -e "${COLOR_GREEN}Boost ${BOOST_VERSION} already installed, skipping${COLOR_OFF}"
    cd "$BWD" || exit
    return
  fi

  echo -e "${COLOR_GREEN}[ INFO ] Downloading Boost ${BOOST_VERSION}${COLOR_OFF}"
  cd "$DEPS_DIR" || exit

  if [ ! -f "$BOOST_ARCHIVE" ]; then
    wget "$BOOST_URL"
  fi

  if [ ! -d "boost_${BOOST_VERSION_UNDERSCORE}" ]; then
    tar -xzf "$BOOST_ARCHIVE"
  fi

  cd "boost_${BOOST_VERSION_UNDERSCORE}" || exit
  BOOST_SRC_DIR=$(pwd)

  if [ ! -f "./b2" ]; then
    ./bootstrap.sh --prefix="$DEPS_DIR" \
      --with-libraries=context,filesystem,program_options,regex,system,thread
  fi

  ./b2 \
    --prefix="$DEPS_DIR" \
    --build-dir="$BOOST_SRC_DIR/build" \
    --without-python \
    variant=release \
    link=static,shared \
    threading=multi \
    cxxflags="-fPIC" \
    -j "$JOBS" \
    install

  echo -e "${COLOR_GREEN}Boost ${BOOST_VERSION} is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_fast_float() {
  FF_DIR="$DEPS_DIR/fast_float"
  FF_TAG="v8.1.0"
  if [ ! -d "$FF_DIR" ]; then
    git clone https://github.com/fastfloat/fast_float.git "$FF_DIR"
  fi
  cd "$FF_DIR"
  git fetch --tags
  git checkout "$FF_TAG"
  mkdir -p "$DEPS_DIR/include/fast_float"
  python3 script/amalgamate.py --output "$DEPS_DIR/include/fast_float/fast_float.h"
  echo -e "${COLOR_GREEN}fast_float is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_gflags() {
  GFLAGS_DIR=$DEPS_DIR/gflags
  GFLAGS_BUILD_DIR=$DEPS_DIR/gflags/build/
  GFLAGS_TAG="v2.2.2"
  if [ ! -d "$GFLAGS_DIR" ]; then
    echo -e "${COLOR_GREEN}[ INFO ] Cloning gflags repo${COLOR_OFF}"
    git clone https://github.com/gflags/gflags.git "$GFLAGS_DIR"
  fi
  cd "$GFLAGS_DIR"
  git fetch --tags
  git checkout "${GFLAGS_TAG}"
  echo -e "${COLOR_GREEN}Building gflags${COLOR_OFF}"
  mkdir -p "$GFLAGS_BUILD_DIR"
  cd "$GFLAGS_BUILD_DIR" || exit
  cmake \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTING=OFF \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}gflags is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_glog() {
  GLOG_DIR=$DEPS_DIR/glog
  GLOG_BUILD_DIR=$DEPS_DIR/glog/build/
  GLOG_TAG="v0.6.0"
  if [ ! -d "$GLOG_DIR" ]; then
    echo -e "${COLOR_GREEN}[ INFO ] Cloning glog repo${COLOR_OFF}"
    git clone https://github.com/google/glog.git "$GLOG_DIR"
  fi
  cd "$GLOG_DIR"
  git fetch --tags
  git checkout "${GLOG_TAG}"
  echo -e "${COLOR_GREEN}Building glog${COLOR_OFF}"
  mkdir -p "$GLOG_BUILD_DIR"
  cd "$GLOG_BUILD_DIR" || exit
  cmake \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_CXX_VISIBILITY_PRESET=default \
    -DCMAKE_C_VISIBILITY_PRESET=default \
    -DBUILD_TESTING=OFF \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}glog is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_fmt() {
  FMT_DIR=$DEPS_DIR/fmt
  FMT_BUILD_DIR=$DEPS_DIR/fmt/build/
  FMT_TAG=$(grep "subdir = " "$PROXYGEN_SRC_DIR/build/fbcode_builder/manifests/fmt" | cut -d "-" -f 2)
  if [ ! -d "$FMT_DIR" ]; then
    git clone https://github.com/fmtlib/fmt.git "$FMT_DIR"
  fi
  cd "$FMT_DIR"
  git fetch --tags
  git checkout "${FMT_TAG}"
  mkdir -p "$FMT_BUILD_DIR"
  cd "$FMT_BUILD_DIR" || exit
  cmake \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DFMT_DOC=OFF \
    -DFMT_TEST=OFF \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}fmt is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_googletest() {
  GTEST_DIR=$DEPS_DIR/googletest
  GTEST_BUILD_DIR=$DEPS_DIR/googletest/build/
  GTEST_TAG=$(grep "subdir = " "$PROXYGEN_SRC_DIR/build/fbcode_builder/manifests/googletest" | cut -d "-" -f 2,3)
  if [ ! -d "$GTEST_DIR" ]; then
    git clone https://github.com/google/googletest.git "$GTEST_DIR"
  fi
  cd "$GTEST_DIR"
  git fetch --tags
  git checkout "${GTEST_TAG}"
  mkdir -p "$GTEST_BUILD_DIR"
  cd "$GTEST_BUILD_DIR" || exit
  cmake \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}googletest is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_zstd() {
  ZSTD_DIR=$DEPS_DIR/zstd
  ZSTD_BUILD_DIR=$DEPS_DIR/zstd/build/cmake/builddir
  ZSTD_TAG=$(grep "subdir = " "$PROXYGEN_SRC_DIR/build/fbcode_builder/manifests/zstd" | cut -d "-" -f 2 | cut -d "/" -f 1)
  if [ ! -d "$ZSTD_DIR" ]; then
    git clone https://github.com/facebook/zstd.git "$ZSTD_DIR"
  fi
  cd "$ZSTD_DIR"
  git fetch --tags
  git checkout "v${ZSTD_TAG}"
  mkdir -p "$ZSTD_BUILD_DIR"
  cd "$ZSTD_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTS=OFF \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Zstd is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_liburing() {
  LIBURING_DIR="${DEPS_DIR}/liburing"
  LIBURING_TAG="liburing-2.12"
  if [ ! -d "$LIBURING_DIR" ]; then
    git clone "https://github.com/axboe/liburing.git" "$LIBURING_DIR"
  fi
  cd "$LIBURING_DIR"
  git fetch --tags
  git checkout "$LIBURING_TAG"
  ./configure --cc=gcc --cxx=g++ --prefix="$DEPS_DIR"
  make -j "$JOBS"
  make liburing.pc
  make install
  echo -e "${COLOR_GREEN}liburing is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_folly() {
  FOLLY_DIR=$DEPS_DIR/folly
  FOLLY_BUILD_DIR=$DEPS_DIR/folly/build/
  if [ ! -d "$FOLLY_DIR" ]; then
    git clone https://github.com/facebook/folly.git "$FOLLY_DIR"
  fi
  synch_dependency_to_commit "$FOLLY_DIR" \
    "$PROXYGEN_SRC_DIR/build/deps/github_hashes/facebook/folly-rev.txt"
  mkdir -p "$FOLLY_BUILD_DIR"
  cd "$FOLLY_BUILD_DIR" || exit
  cmake \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Folly is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_libaegis() {
  LIBAEGIS_DIR=$DEPS_DIR/libaegis
  LIBAEGIS_BUILD_DIR=$DEPS_DIR/libaegis/build/
  LIBAEGIS_TAG="0.4.0"
  if [ ! -d "$LIBAEGIS_DIR" ]; then
    git clone https://github.com/aegis-aead/libaegis.git "$LIBAEGIS_DIR"
  fi
  cd "$LIBAEGIS_DIR"
  git fetch --tags
  git checkout "${LIBAEGIS_TAG}"
  mkdir -p "$LIBAEGIS_BUILD_DIR"
  cd "$LIBAEGIS_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    "${LIBAEGIS_DIR}"
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Libaegis is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_fizz() {
  FIZZ_DIR=$DEPS_DIR/fizz
  FIZZ_BUILD_DIR=$DEPS_DIR/fizz/build/
  if [ ! -d "$FIZZ_DIR" ]; then
    git clone https://github.com/facebookincubator/fizz "$FIZZ_DIR"
  fi
  synch_dependency_to_commit "$FIZZ_DIR" \
    "$PROXYGEN_SRC_DIR/build/deps/github_hashes/facebookincubator/fizz-rev.txt"
  mkdir -p "$FIZZ_BUILD_DIR"
  cd "$FIZZ_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    "$FIZZ_DIR/fizz"
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Fizz is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_wangle() {
  WANGLE_DIR=$DEPS_DIR/wangle
  WANGLE_BUILD_DIR=$DEPS_DIR/wangle/build/
  if [ ! -d "$WANGLE_DIR" ]; then
    git clone https://github.com/facebook/wangle "$WANGLE_DIR"
  fi
  synch_dependency_to_commit "$WANGLE_DIR" \
    "$PROXYGEN_SRC_DIR/build/deps/github_hashes/facebook/wangle-rev.txt"
  mkdir -p "$WANGLE_BUILD_DIR"
  cd "$WANGLE_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    "$WANGLE_DIR/wangle"
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Wangle is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_cares() {
  CARES_DIR=$DEPS_DIR/c-ares
  CARES_BUILD_DIR=$DEPS_DIR/c-ares/build/
  CARES_TAG="v1.34.4"
  if [ ! -d "$CARES_DIR" ]; then
    git clone https://github.com/c-ares/c-ares.git "$CARES_DIR"
  fi
  cd "$CARES_DIR"
  git fetch --tags
  git checkout "${CARES_TAG}"
  mkdir -p "$CARES_BUILD_DIR"
  cd "$CARES_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCARES_BUILD_TESTS=OFF \
    -DCARES_BUILD_TOOLS=OFF \
    ..
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}c-ares is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function setup_mvfst() {
  MVFST_DIR=$DEPS_DIR/mvfst
  MVFST_BUILD_DIR=$DEPS_DIR/mvfst/build/
  if [ ! -d "$MVFST_DIR" ]; then
    git clone https://github.com/facebook/mvfst "$MVFST_DIR"
  fi
  synch_dependency_to_commit "$MVFST_DIR" \
    "$PROXYGEN_SRC_DIR/build/deps/github_hashes/facebook/mvfst-rev.txt"
  mkdir -p "$MVFST_BUILD_DIR"
  cd "$MVFST_BUILD_DIR" || exit
  cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    "$MVFST_DIR"
  make -j "$JOBS"
  make install
  echo -e "${COLOR_GREEN}Mvfst is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

function build_proxygen() {
  echo -e "${COLOR_GREEN}Building Proxygen${COLOR_OFF}"
  PROXYGEN_BUILD_DIR=$DEPS_DIR/proxygen_build
  mkdir -p "$PROXYGEN_BUILD_DIR"
  cd "$PROXYGEN_BUILD_DIR" || exit
  cmake \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_TESTS=OFF \
    "$PROXYGEN_SRC_DIR"

  # Reduce parallelism for proxygen to mitigate OOM risk
  PROXYGEN_JOBS=$(( JOBS / 2 ))
  if [ "$PROXYGEN_JOBS" -lt 1 ]; then
    PROXYGEN_JOBS=1
  fi
  echo -e "${COLOR_GREEN}Building Proxygen with $PROXYGEN_JOBS jobs (reduced from $JOBS)${COLOR_OFF}"
  make -j "$PROXYGEN_JOBS"
  make install
  echo -e "${COLOR_GREEN}Proxygen is installed${COLOR_OFF}"
  cd "$BWD" || exit
}

# --- Main ---

JOBS=8
FETCH_DEPENDENCIES=true
PROXYGEN_SRC_DIR=""
INSTALL_PREFIX=""

while [ $# -gt 0 ]; do
  case $1 in
    -j|--jobs)        shift; JOBS=$1 ;;
    --proxygen-dir)   shift; PROXYGEN_SRC_DIR=$1 ;;
    -p|--prefix)      shift; INSTALL_PREFIX=$1 ;;
    --no-fetch-dependencies) FETCH_DEPENDENCIES=false ;;
    *) echo "Usage: $0 [-j jobs] --proxygen-dir <dir> [-p prefix]"; exit 1 ;;
  esac
  shift
done

if [ -z "$PROXYGEN_SRC_DIR" ]; then
  echo -e "${COLOR_RED}[ ERROR ] --proxygen-dir is required${COLOR_OFF}"
  exit 1
fi

PROXYGEN_SRC_DIR="$(readlink -f "$PROXYGEN_SRC_DIR")"

detect_platform
install_dependencies

BWD=$(pwd)
DEPS_DIR="${INSTALL_PREFIX:-$BWD/deps}"
mkdir -p "$DEPS_DIR"

setup_boost
setup_fast_float
setup_gflags
setup_glog
setup_fmt
setup_googletest
setup_zstd
if [ "$DISTRO" = "ubuntu" ]; then
  setup_liburing
fi
setup_folly
setup_libaegis
setup_fizz
setup_wangle
setup_mvfst
setup_cares
build_proxygen

echo -e "${COLOR_GREEN}All dependencies and proxygen are installed in $DEPS_DIR${COLOR_OFF}"
