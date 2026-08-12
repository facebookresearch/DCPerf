#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

# Newer OS/FW images ship CMake >= 4.0, which removed compatibility with
# cmake_minimum_required(VERSION < 3.5). Several pinned deps built here
# (e.g. gflags 2.2.2, glog 0.4.0 via folly getdeps; aom/ffmpeg CMake)
# still declare old minimums and abort with "Compatibility with CMake
# < 3.5 has been removed". Ask CMake to assume a 3.5 policy baseline for
# those old projects. Harmless / no-op on CMake 3.x.
export CMAKE_POLICY_VERSION_MINIMUM=3.5

BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TAO_BENCH_ROOT/../..")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"
TAO_BENCH_ROOT="${BENCHPRESS_ROOT}/benchmarks/tao_bench"
TAO_BENCH_DEPS="${TAO_BENCH_ROOT}/build-deps"
FOLLY_BUILD_ROOT="${TAO_BENCH_ROOT}/build-folly"

if [ -z "${OPENSSL_BRANCH+x}" ]; then
    OPENSSL_BRANCH="openssl-3.3.2"
fi

source "${COMMON_DIR}/os-distro.sh"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"
GLOG_NAME="glog-devel"
# CentOS 10 dropped PCRE1 (EOL); only pcre2-devel is available there, so install
# pcre2-devel and build PCRE1 from source below (memtier needs PCRE1). libevent
# 2.1.8 fails to build under GCC 14+ (implicit arc4random_addrandom); 2.1.12
# fixed it, so bump the branch on el10.
PCRE_NAME="pcre-devel"
LIBEVENT_BRANCH="release-2.1.8-stable"

if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -eq 8 ]; then
    GLOG_NAME="glog-devel-0.3.5-5.el8"
elif [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -eq 9 ]; then
    GLOG_NAME="glog-devel-0.3.5-15.el9"
elif [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -ge 10 ]; then
    PCRE_NAME="pcre2-devel"
    LIBEVENT_BRANCH="release-2.1.12-stable"
else
    echo "Warning: unsupported platform ${LINUX_DIST_ID}-${LINUX_DIST_ID}"
fi

# CentOS containers run as root but with root locked and no sudo PAM
# setup until the workflow adds it — `sudo` fails there with
# "account validation failure, is your account locked?" on dnf installs.
SUDO=""
if distro_is_like centos; then
  if sudo -n true 2>/dev/null; then
    SUDO="sudo"
  else
    SUDO=""
  fi
elif distro_is_like ubuntu; then
  SUDO="sudo"
fi

if distro_is_like centos; then
  ${SUDO} dnf install -y cmake iperf3 autoconf automake \
    libevent-devel openssl openssl-devel \
    zlib-devel bzip2-devel xz-devel lz4-devel libzstd-devel \
    snappy-devel libaio-devel libunwind-devel patch \
    double-conversion-devel libsodium-devel \
    gflags-devel-2.2.2 fmt-devel perl libtool ${PCRE_NAME} \
    git python3-devel binutils binutils-devel ${GLOG_NAME}
elif distro_is_like ubuntu; then
  ${SUDO} apt install -y cmake iperf3 autoconf automake flex bison \
    libevent-dev openssl libssl-dev \
    libzstd-dev lz4 liblz4-dev xzip libsnappy-dev zlib1g-dev bzip2 \
    libaio-dev libunwind-dev patch libghc-double-conversion-dev \
    libsodium-dev  libfmt-dev libtool perl git \
    libgoogle-glog-dev python3-dev pkg-config \
    git libpcre3 libpcre3-dev libgflags2.2 libgflags-dev
else
    echo "Warning: unsupported OS distro - $LINUX_DIST_ID"
fi

# Installing dependencies
mkdir -p "${TAO_BENCH_DEPS}"
pushd "${TAO_BENCH_ROOT}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    ${SUDO} ln -s /usr/bin/cmake /usr/local/bin/cmake
fi

# Install openssl
if ! [ -d "openssl" ]; then
    git clone --branch "${OPENSSL_BRANCH}" --depth 1 https://github.com/openssl/openssl.git
    pushd openssl/
    ./config --prefix="${TAO_BENCH_DEPS}" --libdir=lib
    make -j"$(nproc)"
    make install
    popd
else
    echo "[SKIPPED] OpenSSL (${OPENSSL_BRANCH})"
fi

# Install libevent
if ! [ -d "libevent" ]; then
    git clone --branch "${LIBEVENT_BRANCH}" https://github.com/libevent/libevent
    pushd libevent/
    ./autogen.sh
    ./configure --prefix="${TAO_BENCH_DEPS}" PKG_CONFIG_PATH="${TAO_BENCH_DEPS}/lib/pkgconfig" \
        LDFLAGS="-L${TAO_BENCH_DEPS}/lib" CPPFLAGS="-I${TAO_BENCH_DEPS}/include"
    make -j"$(nproc)"
    make install
    popd
else
    echo "[SKIPPED] libevent-2.1.8"
fi

# Install PCRE1 (CentOS 10 dropped the pcre-devel package; memtier_benchmark's
# configure requires PCRE1, not PCRE2, so build it from source into the deps dir).
if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -ge 10 ]; then
    if ! [ -f "${TAO_BENCH_DEPS}/lib/libpcre.so" ] && ! [ -f "${TAO_BENCH_DEPS}/lib/libpcre.a" ]; then
        rm -rf pcre-8.45
        curl -fL "https://sourceforge.net/projects/pcre/files/pcre/8.45/pcre-8.45.tar.gz/download" -o pcre-8.45.tar.gz
        tar -xzf pcre-8.45.tar.gz
        pushd pcre-8.45
        ./configure --prefix="${TAO_BENCH_DEPS}"
        make -j"$(nproc)"
        make install
        popd
    else
        echo "[SKIPPED] pcre-8.45"
    fi
fi

# Installing folly
if ! [ -d "folly" ]; then
    git clone https://github.com/facebook/folly
else
    echo "[DOWNLOADED] folly"
fi
pushd folly
git checkout v2024.06.24.00
sed -i 's/FOLLY_ALWAYS_INLINE//g' "${TAO_BENCH_ROOT}/folly/folly/experimental/symbolizer/StackTrace.cpp"
# zlib.net only serves the current release; older tarballs are moved to
# /fossils/,
ZLIB_MANIFEST="${TAO_BENCH_ROOT}/folly/build/fbcode_builder/manifests/zlib"
if [ -f "${ZLIB_MANIFEST}" ]; then
    sed -i -E 's#https://zlib\.net/(fossils/)?zlib-(.*)\.tar\.gz#https://github.com/madler/zlib/archive/refs/tags/v\2.tar.gz#' "${ZLIB_MANIFEST}"
    sed -i 's/9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23/17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c/' "${ZLIB_MANIFEST}"
fi
OPENSSL_ROOT_DIR="${TAO_BENCH_DEPS}" ./build/fbcode_builder/getdeps.py --allow-system-packages build \
    --extra-cmake-defines '{"CMAKE_LIBRARY_ARCHITECTURE": "aarch64"}' \
    --scratch-path "${FOLLY_BUILD_ROOT}"
popd

# === Build and install memcached (tao_bench_server) ===
rm -rf memcached-1.6.5
curl http://www.memcached.org/files/memcached-1.6.5.tar.gz > memcached-1.6.5.tar.gz
tar -zxf memcached-1.6.5.tar.gz
pushd memcached-1.6.5
# We'll need to run autogen.sh if config.h.in does not exist in memcached's source
if ! [ -f "config.h.in" ]; then
    ./autogen.sh
fi
# Patch w/ Tao Bench changes
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0001-tao_bench_memcached.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0002-tao_bench_memcached_oom_handling.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0003-tao_bench_thread_pool_naming.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0006-tao_bench_slow_thread_use_semaphore.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0007-tao_bench_smart_nanosleep.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0008-tao_bench_count_nanosleeps.diff"

# Find the path to folly and fmt
FOLLY_INSTALLED_PATH="${FOLLY_BUILD_ROOT}/installed/folly"
FMT_INSTALLED_PATH="$(find "${FOLLY_BUILD_ROOT}/installed" -maxdepth 1 -name "fmt-*" | head -n1)"

if ! [ -d "${FOLLY_INSTALLED_PATH}/lib64" ]; then
    ln -s -f "${FOLLY_INSTALLED_PATH}/lib" "${FOLLY_INSTALLED_PATH}/lib64"
fi
if ! [ -d "${FMT_INSTALLED_PATH}/lib64" ]; then
    ln -s -f "${FMT_INSTALLED_PATH}/lib" "${FMT_INSTALLED_PATH}/lib64"
fi
if ! [ -d "${FMT_INSTALLED_PATH}" ]; then
    echo "Cannot find path to fmt" && exit 1
fi

# Build and install
if ! [ -f "/usr/bin/aclocal-1.16" ]; then
    ${SUDO} ln -s /usr/bin/aclocal /usr/bin/aclocal-1.16
fi
if ! [ -f "/usr/bin/automake-1.16" ]; then
    ${SUDO} ln -s /usr/bin/automake /usr/bin/automake-1.16
fi

./configure --with-folly="${FOLLY_INSTALLED_PATH}" --with-fmt="${FMT_INSTALLED_PATH}" \
            --with-libssl="${TAO_BENCH_DEPS}" \
            --disable-coverage --enable-tls
make -j"$(nproc)"

if [ -L /usr/bin/aclocal-1.16 ]; then
    ${SUDO} rm -f /usr/bin/aclocal-1.16
fi
if [ -L /usr/bin/automake-1.16 ]; then
    ${SUDO} rm -f /usr/bin/automake-1.16
fi

cp memcached "${TAO_BENCH_ROOT}/tao_bench_server"
cp "${BPKGS_TAO_BENCH_ROOT}/db_items.json" "${TAO_BENCH_ROOT}/"
cp "${BPKGS_TAO_BENCH_ROOT}/leader_sizes.json" "${TAO_BENCH_ROOT}/"
cp -r "${COMMON_DIR}/affinitize" "${TAO_BENCH_ROOT}/"
popd

# === Build and install memtier_client (tao_bench_client) ===
pushd "${TAO_BENCH_ROOT}"
# Download memtier benchmark
rm -rf memtier_client
git clone https://github.com/RedisLabs/memtier_benchmark memtier_client
pushd memtier_client
# Latest commit as of 06/15/2023
git checkout 7bea7c63c5e95fea061366b95494bf730c5ca0d4
# Apply the patch
git apply --check "${BPKGS_TAO_BENCH_ROOT}/0005-tao_bench_client_memtier_20230615.diff" && \
    git apply "${BPKGS_TAO_BENCH_ROOT}/0005-tao_bench_client_memtier_20230615.diff"
# Build and install
autoreconf --force --install
# On CentOS 10 point memtier at the PCRE1 we built into TAO_BENCH_DEPS (rpath so
# the binary resolves libpcre at runtime). Empty on other platforms.
MEMTIER_LDFLAGS=""
MEMTIER_CPPFLAGS=""
if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -ge 10 ]; then
    MEMTIER_LDFLAGS="-L${TAO_BENCH_DEPS}/lib -Wl,-rpath,${TAO_BENCH_DEPS}/lib"
    MEMTIER_CPPFLAGS="-I${TAO_BENCH_DEPS}/include"
fi
PKG_CONFIG_PATH="${TAO_BENCH_DEPS}/lib/pkgconfig" LDFLAGS="${MEMTIER_LDFLAGS}" CPPFLAGS="${MEMTIER_CPPFLAGS}" ./configure --enable-tls
make -j"$(nproc)" || ( automake --add-missing && make -j"$(nproc)" )
cp memtier_benchmark "${TAO_BENCH_ROOT}/tao_bench_client"
popd # memtier_client
popd # $TAO_BENCH_ROOT

# Extract certificates
tar -zxf "${COMMON_DIR}/certs.tar.gz" -C "${TAO_BENCH_ROOT}/"
popd
