#!/bin/bash
set -Eeuo pipefail

BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TAO_BENCH_ROOT/../..")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"
TAO_BENCH_ROOT="${BENCHPRESS_ROOT}/benchmarks/tao_bench"
TAO_BENCH_DEPS="${TAO_BENCH_ROOT}/build-deps"
FOLLY_BUILD_ROOT="${TAO_BENCH_ROOT}/build-folly"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"
GLOG_NAME="glog-devel"

if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -eq 8 ]; then
    GLOG_NAME="glog-devel-0.3.5-5.el8"
elif [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -eq 9 ]; then
    GLOG_NAME="glog-devel-0.3.5-15.el9"
else
    echo "Warning: unsupported platform ${LINUX_DIST_ID}-${LINUX_DIST_ID}"
fi

sudo dnf install -y cmake autoconf automake \
    libevent-devel openssl openssl-devel \
    zlib-devel bzip2-devel xz-devel lz4-devel libzstd-devel \
    snappy-devel libaio-devel libunwind-devel patch \
    double-conversion-devel libsodium-devel \
    gflags-devel-2.2.2 fmt-devel perl libtool pcre-devel \
    git python3-devel ${GLOG_NAME}

# Installing dependencies
mkdir -p "${TAO_BENCH_DEPS}"
pushd "${TAO_BENCH_ROOT}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    sudo ln -s /usr/bin/cmake3 /usr/local/bin/cmake
fi

# Install openssl
if ! [ -d "openssl" ]; then
    git clone --branch OpenSSL_1_1_1b --depth 1 https://github.com/openssl/openssl.git
    pushd openssl/
    ./config --prefix="${TAO_BENCH_DEPS}"
    make -j"$(nproc)"
    make install
    popd
else
    echo "[SKIPPED] openssl_1_1_1b"
fi

# Install libevent
if ! [ -d "libevent" ]; then
    git clone --branch release-2.1.8-stable https://github.com/libevent/libevent
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

# Installing folly
if ! [ -d "folly" ]; then
    git clone https://github.com/facebook/folly
else
    echo "[DOWNLOADED] folly"
fi
pushd folly
git checkout v2023.02.27.00
sed -i 's/FOLLY_ALWAYS_INLINE//g' "${TAO_BENCH_ROOT}/folly/folly/experimental/symbolizer/StackTrace.cpp"
OPENSSL_ROOT_DIR="${TAO_BENCH_DEPS}" ./build/fbcode_builder/getdeps.py --allow-system-packages build \
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
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/tao_bench_memcached_0001.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0002-tao_bench_memcached_oom_handling.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0003-tao_bench_thread_pool_naming.diff"

# Find the path to folly and fmt
FOLLY_INSTALLED_PATH="${FOLLY_BUILD_ROOT}/installed/folly"
FMT_INSTALLED_PATH="$(find "${FOLLY_BUILD_ROOT}/installed" -maxdepth 1 -name "fmt-*" | head -n1)"

if ! [ -d "${FOLLY_INSTALLED_PATH}/lib64" ]; then
    ln -s -f "${FOLLY_INSTALLED_PATH}/lib" "${FOLLY_INSTALLED_PATH}/lib64"
fi
if ! [ -d "${FMT_INSTALLED_PATH}" ]; then
    echo "Cannot find path to fmt" && exit 1
fi

# Build and install
if ! [ -f "/usr/bin/aclocal-1.16" ]; then
    sudo ln -s /usr/bin/aclocal /usr/bin/aclocal-1.16
fi
if ! [ -f "/usr/bin/automake-1.16" ]; then
    sudo ln -s /usr/bin/automake /usr/bin/automake-1.16
fi

./configure --with-folly="${FOLLY_INSTALLED_PATH}" --with-fmt="${FMT_INSTALLED_PATH}" \
            --with-libssl="${TAO_BENCH_DEPS}" \
            --disable-coverage --enable-tls
make -j"$(nproc)"

if [ -L /usr/bin/aclocal-1.16 ]; then
    sudo rm -f /usr/bin/aclocal-1.16
fi
if [ -L /usr/bin/automake-1.16 ]; then
    sudo rm -f /usr/bin/automake-1.16
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
./configure --enable-tls
make -j"$(nproc)" || ( automake --add-missing && make -j"$(nproc)" )
cp memtier_benchmark "${TAO_BENCH_ROOT}/tao_bench_client"
popd # memtier_client
popd # $TAO_BENCH_ROOT

# Extract certificates
tar -zxf "${COMMON_DIR}/certs.tar.gz" -C "${TAO_BENCH_ROOT}/"
popd
