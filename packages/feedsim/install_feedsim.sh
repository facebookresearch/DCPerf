#!/bin/bash

set -Eeuo pipefail
# trap cleanup SIGINT SIGTERM ERR EXIT

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

source "${BENCHPRESS_ROOT}/packages/common/os-distro.sh"

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

ARCH="$(uname -p)"
if [ "$ARCH" = "aarch64" ]; then
  if distro_is_like ubuntu; then
    "${FEEDSIM_ROOT}"/install_feedsim_aarch64_ubuntu.sh
     exit $?
  else
    "${FEEDSIM_ROOT}"/install_feedsim_aarch64.sh
    exit $?
  fi
fi
if distro_is_like ubuntu && [ "$(uname -p)" = "x86_64" ]; then
  "${FEEDSIM_ROOT}"/install_feedsim_x86_64_ubuntu.sh
  exit $?
fi
dnf install -y ninja-build flex bison git texinfo binutils-devel \
    libsodium-devel libunwind-devel bzip2-devel double-conversion-devel \
    libzstd-devel lz4-devel xz-devel snappy-devel libtool bzip2 openssl-devel \
    zlib-devel libdwarf libdwarf-devel libaio-devel libatomic patch jq

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
cp -r "${BENCHPRESS_ROOT}/packages/feedsim/third_party" "${FEEDSIM_ROOT_SRC}"
mv "${FEEDSIM_THIRD_PARTY_SRC}/src" "${FEEDSIM_ROOT_SRC}/src"
cd "${FEEDSIM_THIRD_PARTY_SRC}"

# Installing cmake-3.14.5

if ! [ -d "cmake-3.14.5" ]; then
    wget "https://github.com/Kitware/CMake/releases/download/v3.14.5/cmake-3.14.5.tar.gz"
    tar -zxf "cmake-3.14.5.tar.gz"
    cd "cmake-3.14.5"
    mkdir staging
    ./bootstrap --parallel=8 --prefix="$(pwd)/staging"
    make -j8
    make install
    cd ../
else
    msg "[SKIPPED] cmake-3.14.5"
fi

export PATH="${FEEDSIM_THIRD_PARTY_SRC}/cmake-3.14.5/staging/bin:${PATH}"

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
if ! [ -d "boost_1_71_0" ] && ! grep -i 'centos stream release 9' /etc/*-release; then
    wget "https://boostorg.jfrog.io/artifactory/main/release/1.71.0/source/boost_1_71_0.tar.gz"
    tar -xzf "boost_1_71_0.tar.gz"
    cd "boost_1_71_0"
    ./bootstrap.sh --without-libraries=python
    ./b2 install
    cd ../
elif grep -i 'centos stream release 9' /etc/*-release; then
    # On CentOS 9 let's just use boost 1.75 that comes with the system
    sudo dnf install -y boost-devel
else
    msg "[SKIPPED] boost_1_71_0"
fi


# Installing gflags
if ! [ -d "gflags-2.2.2" ]; then
    wget "https://github.com/gflags/gflags/archive/refs/tags/v2.2.2.tar.gz" -O "gflags-2.2.2.tar.gz"
    tar -xzf "gflags-2.2.2.tar.gz"
    cd "gflags-2.2.2"
    mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release ../
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
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release ../
    make -j8
    make install
    cd ../../
else
    msg "[SKIPPED] glog-0.4.0"
fi

# Installing JEMalloc
if ! [ -d "jemalloc-5.2.1" ]; then
    wget "https://github.com/jemalloc/jemalloc/releases/download/5.2.1/jemalloc-5.2.1.tar.bz2"
    bunzip2 "jemalloc-5.2.1.tar.bz2"
    tar -xvf "jemalloc-5.2.1.tar"
    cd "jemalloc-5.2.1"
    ./configure --enable-prof --enable-prof-libunwind
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] jemalloc-5.2.1"
fi

# Installing libevent
if ! [ -d "libevent-2.1.11-stable" ]; then
    wget "https://github.com/libevent/libevent/releases/download/release-2.1.11-stable/libevent-2.1.11-stable.tar.gz"
    tar -xzf "libevent-2.1.11-stable.tar.gz"
    cd "libevent-2.1.11-stable"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] libevent-2.1.11-stable"
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

# If running on CentOS Stream 9, apply compatilibity patches to folly, rsocket and wangle
# TODO: This is a temporary fix. In the long term we should seek to have feedsim
# support the up-to-date version of these dependencies
REPOS_TO_PATCH=(folly wangle rsocket-cpp)
if grep -i 'centos stream release 9' /etc/*-release >/dev/null 2>&1; then
    for repo in "${REPOS_TO_PATCH[@]}"; do
        pushd "third_party/$repo" || exit 1
        git apply --check "${FEEDSIM_ROOT}/patches/centos-9-compatibility/${repo}.diff" && \
            git apply "${FEEDSIM_ROOT}/patches/centos-9-compatibility/${repo}.diff"
        popd || exit 1
    done
fi

mkdir -p build && cd build/

# Build FeedSim
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG }"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic"

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    ../
ninja -v
