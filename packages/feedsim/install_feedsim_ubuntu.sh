#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail
# trap cleanup SIGINT SIGTERM ERR EXIT

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

cleanup() {
  trap - SIGINT SIGTERM ERR EXIT
}


msg() {
  echo >&2 -e "${1-}"
}

die() {
  local msg=$1
  local code=${2-1} # default exit status 1
  msg "$msg"
  exit "$code"
}

#dnf install -y cmake ninja-build flex bison git texinfo binutils-devel \
#    libunwind-devel bzip2-devel libsodium-devel double-conversion-devel \
#    libzstd-devel lz4-devel xz-devel snappy-devel libtool openssl-devel \
#    zlib-devel libdwarf-devel libaio-devel libatomic patch perl
apt install -y bc cmake ninja-build flex bison texinfo binutils-dev \
    libunwind-dev bzip2 libbz2-dev libsodium-dev libghc-double-conversion-dev \
    libzstd-dev lz4 liblz4-dev xzip libsnappy-dev libtool libssl-dev \
    zlib1g-dev libdwarf-dev libaio-dev libatomic1 patch perl libiberty-dev \
    libfmt-dev sysstat jq xxhash libxxhash-dev


# Creates feedsim directory under benchmarks/
mkdir -p "${BENCHPRESS_ROOT}/benchmarks/feedsim"
cd "${BENCHPRESS_ROOT}/benchmarks"

# Copy run.sh template (overwrite)
cp "${BENCHPRESS_ROOT}/packages/feedsim/run.sh" "${FEEDSIM_ROOT_SRC}/run.sh"
cp "${BENCHPRESS_ROOT}/packages/feedsim/run-feedsim-multi.sh" "${FEEDSIM_ROOT_SRC}/run-feedsim-multi.sh"
# Set as executable
chmod u+x "${FEEDSIM_ROOT_SRC}/run.sh"
chmod u+x "${FEEDSIM_ROOT_SRC}/run-feedsim-multi.sh"

msg "Installing third-party dependencies..."
mkdir -p "${FEEDSIM_THIRD_PARTY_SRC}"
if ! [ -d "${FEEDSIM_ROOT_SRC}/src" ]; then
    cp -r "${BENCHPRESS_ROOT}/packages/feedsim/third_party/src" "${FEEDSIM_ROOT_SRC}/"
else
    msg "[SKIPPED] copying feedsim src"
fi
cd "${FEEDSIM_THIRD_PARTY_SRC}"

# Installing cmake-4.0.3

if ! [ -d "cmake-4.0.3" ]; then
    wget "https://github.com/Kitware/CMake/releases/download/v4.0.3/cmake-4.0.3.tar.gz"
    tar -zxf "cmake-4.0.3.tar.gz"
    cd "cmake-4.0.3"
    mkdir staging
    ./bootstrap --parallel=8 --prefix="$(pwd)/staging"
    make -j8
    make install
    cd ../
else
    msg "[SKIPPED] cmake-4.0.3"
fi

export PATH="${FEEDSIM_THIRD_PARTY_SRC}/cmake-4.0.3/staging/bin:${PATH}"

git clone https://github.com/fastfloat/fast_float.git
cd fast_float
mkdir build && cd build
cmake ..
make
make install
cd ../

# Installing gengetopt
if ! [ -d "gengetopt-2.23" ]; then
    wget "https://ftp.gnu.org/gnu/gengetopt/gengetopt-2.23.tar.xz"
    tar -xf "gengetopt-2.23.tar.xz"
    cd "gengetopt-2.23"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] gengetopt-2.23"
fi

# Installing Boost
if ! [ -d "boost_1_88_0" ]; then
    wget "https://archives.boost.io/release/1.88.0/source/boost_1_88_0.tar.gz"
    tar -xzf "boost_1_88_0.tar.gz"
    cd "boost_1_88_0"
    ./bootstrap.sh --without-libraries=python
    ./b2 install
    cd ../
else
    msg "[SKIPPED] boost_1_88_0"
fi


# Installing gflags
if ! [ -d "gflags-2.2.2" ]; then
    wget "https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz" -O "gflags-2.2.2.tar.gz"
    tar -xzf "gflags-2.2.2.tar.gz"
    cd "gflags-2.2.2"
    mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ../
    make -j8
    make install
    cd ../../
else
    msg "[SKIPPED] gflags-2.2.2"
fi

# Installing glog
if ! [ -d "glog-0.4.0" ]; then
    wget "https://github.com/google/glog/archive/refs/tags/v0.4.0.tar.gz" -O "glog-0.4.0.tar.gz"
    tar -xzf "glog-0.4.0.tar.gz"
    cd "glog-0.4.0"
    mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ../
    make -j8
    make install
    cd ../../
else
    msg "[SKIPPED] glog-0.4.0"
fi

# Installing JEMalloc
if ! [ -d "jemalloc-5.3.0" ]; then
    wget "https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2"
    bunzip2 "jemalloc-5.3.0.tar.bz2"
    tar -xvf "jemalloc-5.3.0.tar"
    cd "jemalloc-5.3.0"
    ./configure --enable-prof --enable-prof-libunwind
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] jemalloc-5.3.0"
fi

# Installing libevent
if ! [ -d "libevent-2.1.11-stable" ]; then
    wget "https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz"
    tar -xzf "libevent-2.1.12-stable.tar.gz"
    cd "libevent-2.1.12-stable"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] libevent-2.1.12-stable"
fi

msg "Installing third-party dependencies ... DONE"


# Installing FeedSim
cd "${FEEDSIM_ROOT_SRC}"

cd "src"

# Populate third party submodules
while read -r submod;
do
    REPO="$(echo "$submod" | cut -d ' ' -f 1)"
    COMMIT="$(echo "$submod" | cut -d ' ' -f 2)"
    SUBMOD_DIR="$(echo "$submod" | cut -d ' ' -f 3)"
    mkdir -p "${SUBMOD_DIR}"
    git clone "${REPO}" "${SUBMOD_DIR}"
    pushd "${SUBMOD_DIR}"
    git checkout "${COMMIT}"
    popd
done < "${FEEDSIM_ROOT}/submodules.txt"

# Patch fizz for OpenSSL 3.0 compatibility
if [ -f "third_party/fizz/fizz/tool/FizzServerCommand.cpp" ]; then
    # Replace EVP_PKEY_cmp with EVP_PKEY_eq
    sed -i 's/EVP_PKEY_cmp(pubKey.get(), key.get()) == 1/EVP_PKEY_eq(pubKey.get(), key.get())/g' "third_party/fizz/fizz/tool/FizzServerCommand.cpp"
fi

mkdir -p build && cd build/

# Build FeedSim
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG }"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic"

BP_CC=gcc
BP_CXX=g++

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS -DFMT_HEADER_ONLY=1" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    ../

sed -i 's/lib64/lib/' build.ninja
ninja -v -j1
