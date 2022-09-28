#!/bin/bash
set -Eeuo pipefail

# FIXME(cltorres): Copy/link bpkgs benchmark contents into the BP_TMP automatically.
BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TAO_BENCH_ROOT/../..")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"
TAO_BENCH_ROOT="${BENCHPRESS_ROOT}/benchmarks/tao_bench"
TAO_BENCH_DEPS="${TAO_BENCH_ROOT}/build-deps"
FOLLY_BUILD_ROOT="${TAO_BENCH_ROOT}/build-folly"

sudo yum install -y gcc gcc-c++ cmake3 autoconf automake \
    libevent-devel openssl openssl-devel \
    zlib-devel bzip2-devel xz-devel lz4-devel libzstd-devel \
    snappy-devel libaio-devel libunwind-devel patch
sudo yum remove -y libdwarf-devel

sudo amazon-linux-extras install -y epel
sudo yum install -y double-conversion-devel libsodium-devel gflags-devel fmt-devel
sudo yum remove -y glog-devel

# Installing dependencies
mkdir -p "${TAO_BENCH_DEPS}"
pushd "${TAO_BENCH_ROOT}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    sudo ln -s /usr/bin/cmake3 /usr/local/bin/cmake
fi

# Install glog
if ! [ -d "glog-0.4.0" ]; then
    wget "https://github.com/google/glog/archive/refs/tags/v0.4.0.tar.gz" -O "glog-0.4.0.tar.gz"
    tar -xzf "glog-0.4.0.tar.gz"
    mkdir -p "glog-0.4.0/build"
    pushd "glog-0.4.0/build"
    cmake ../ \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release
    make -j"$(nproc)"
    sudo make install
    popd
else
    echo "[SKIPPED] glog-0.4.0"
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

# Installing folly
if ! [ -d "folly" ]; then
    git clone https://github.com/facebook/folly
else
    echo "[DOWNLOADED] folly"
fi
pushd folly
sed -i 's/FOLLY_ALWAYS_INLINE//g' "${TAO_BENCH_ROOT}/folly/folly/experimental/symbolizer/StackTrace.cpp"
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
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/tao_bench_memcached_0001.diff"

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
# Copy memtier_client source
rm -rf "${TAO_BENCH_ROOT}/memtier_client"
cp -r "${COMMON_DIR}/memtier_client" "${TAO_BENCH_ROOT}/"
pushd memtier_client

# Build and install
autoreconf --force --install
./configure --enable-tls
make -j"$(nproc)" || ( automake --add-missing && make -j"$(nproc)" )
cp memtier_benchmark "${TAO_BENCH_ROOT}/tao_bench_client"
popd

# Extract certificates
tar -zxf "${COMMON_DIR}/certs.tar.gz" -C "${TAO_BENCH_ROOT}/"
popd
