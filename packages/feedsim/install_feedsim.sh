#!/bin/bash

set -Eeuo pipefail
trap cleanup SIGINT SIGTERM ERR EXIT

# Constants
BENCHPRESS_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_MANIFOLD_BUCKET='benchpress_artifacts/tree'
BENCHPRESS_TEMPLATES="${BENCHPRESS_ROOT}/templates"
FEEDSIM_ROOT_MANIFOLD="${BENCHPRESS_MANIFOLD_BUCKET}/feedsim"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"

cleanup() {
  trap - SIGINT SIGTERM ERR EXIT
  unset https_proxy
  kill %socat
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


dnf install -y cmake-3.14.5 ninja-build flex bison git texinfo binutils-devel \
    libsodium-devel libunwind-devel bzip2-devel double-conversion-devel \
    libzstd-devel lz4-devel-1.8.3 xz-devel snappy-devel libtool bzip2 openssl-devel \
    zlib-devel libdwarf libdwarf-devel libaio-devel fb-fwdproxy-config socat \
    libatomic-static patch


mkdir -p "${BENCHPRESS_ROOT}/benchmarks"
cd "${BENCHPRESS_ROOT}/benchmarks"

# Recursively download feedsim source code
# Creates feedsim directory under benchmarks/
manifold getr "${FEEDSIM_ROOT_MANIFOLD}"

# Copy run.sh template (overwrite)
cp "${BENCHPRESS_TEMPLATES}/feedsim/run.sh" "${FEEDSIM_ROOT_SRC}/run.sh"
# Set as executable
chmod u+x "${FEEDSIM_ROOT_SRC}/run.sh"

msg "Installing third-party dependencies..."
cd "${FEEDSIM_THIRD_PARTY_SRC}"

# Installing gengetopt
if ! [ -d "gengetopt-2.23" ]; then
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
if ! [ -d "boost_1_71_0" ]; then
    tar -xzf "boost_1_71_0.tar.gz"
    cd "boost_1_71_0"
    ./bootstrap.sh --without-libraries=python
    ./b2 install
    cd ../
else
    msg "[SKIPPED] boost_1_71_0"
fi


# Installing gflags
if ! [ -d "gflags-2.2.2" ]; then
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

tar -xzf "fb-oldisim.tar.gz"
cd "oldisim"
mkdir -p build && cd build/

# Set up the fwdproxy
export no_proxy=".fbcdn.net,.facebook.com,.thefacebook.com,.tfbnw.net,.fb.com,.fburl.com,.facebook.net,.sb.fbsbx.com,localhost"
socat tcp-listen:9876,fork \
    openssl-connect:fwdproxy:8082,cert=/var/facebook/x509_identities/server.pem,cafile=/var/facebook/rootcanal/ca.pem,commonname=svc:fwdproxy &

# Build FeedSim
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG }"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic"

HTTP_PROXY=localhost:9876 HTTPS_PROXY=localhost:9876 cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    ../
ninja -v
