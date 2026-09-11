#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
LIBTORCH_VERSION="${LIBTORCH_VERSION:-2.13.0}"
# When 1, fetch LibTorch by extracting it from the prebuilt torch CPU wheel
# (download.pytorch.org/whl/cpu) instead of the libtorch-shared-with-deps zip.
# Required for LibTorch >=2.9 (2.13.0 and later publish a wheel but no
# standalone zip); harmless for older versions. Default 1 pairs with the
# LIBTORCH_VERSION=2.13.0 default so the out-of-box install works without
# additional env overrides.
LIBTORCH_FROM_WHEEL="${LIBTORCH_FROM_WHEEL:-1}"
DLRM_MODEL_URL="https://github.com/facebookresearch/DCPerf-datasets/releases/download/feedsim-dlrm/dlrm_small.tar.gz"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

msg() {
  echo >&2 -e "${1-}"
}

die() {
  local msg=$1
  local code=${2-1} # default exit status 1
  msg "$msg"
  exit "$code"
}

apt install -y bc cmake ninja-build flex bison texinfo binutils-dev \
    libunwind-dev bzip2 libbz2-dev libsodium-dev libghc-double-conversion-dev \
    libzstd-dev lz4 liblz4-dev xzip libsnappy-dev libtool libssl-dev \
    zlib1g-dev libdwarf-dev libaio-dev libatomic1 patch perl libiberty-dev \
    sysstat jq xxhash libxxhash-dev unzip rsync curl

# Install liburing >= 2.6 from source. Ubuntu's apt-shipped liburing (0.7 on
# 20.04, 2.1 on 22.04) is older than folly's minimum, so folly's io_uring
# integration links `-luring` against a nonexistent library. Build tag
# liburing-2.12 upstream and install into /usr/local so folly's `find_library`
# picks it up ahead of the system one.
if ! ldconfig -p 2>/dev/null | grep -q "liburing.so.2"; then
    msg "Building liburing 2.12 from source..."
    LIBURING_BUILD_DIR="${FEEDSIM_THIRD_PARTY_SRC}/liburing_build"
    mkdir -p "${LIBURING_BUILD_DIR}"
    if ! [ -d "${LIBURING_BUILD_DIR}/liburing" ]; then
        git clone --depth 1 --branch liburing-2.12 \
            https://github.com/axboe/liburing.git \
            "${LIBURING_BUILD_DIR}/liburing"
    fi
    (
        cd "${LIBURING_BUILD_DIR}/liburing"
        ./configure --prefix=/usr/local
        make -j"$(nproc)"
        make install
    )
    ldconfig
fi

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
# Sync feedsim source with --delete so `./benchpress install -f` actually picks up
# source changes. The previous "skip if dir exists" guard meant -f never refreshed
# source files. rsync with trailing slashes does file-level overwrite + deletion of
# removed files.
#
# CRITICAL --exclude=third_party: subsequent install steps git-clone submodules
# (cereal, fbthrift, folly, wangle, fizz, mvfst, ...) INTO ${FEEDSIM_ROOT_SRC}/src/
# third_party/<submod>/. Without this exclude, rsync --delete wipes those submodule
# directories on every re-install, forcing a re-clone. The top-level
# src/third_party/CMakeLists.txt is copied separately below.
mkdir -p "${FEEDSIM_ROOT_SRC}/src" "${FEEDSIM_THIRD_PARTY_SRC}"
rsync -a --delete --exclude=third_party \
    "${BENCHPRESS_ROOT}/packages/feedsim/third_party/src/" \
    "${FEEDSIM_ROOT_SRC}/src/"
mkdir -p "${FEEDSIM_ROOT_SRC}/src/third_party"
cp -f "${BENCHPRESS_ROOT}/packages/feedsim/third_party/src/third_party/CMakeLists.txt" \
    "${FEEDSIM_ROOT_SRC}/src/third_party/CMakeLists.txt"
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

if ! [ -d "fast_float" ]; then
    git clone https://github.com/fastfloat/fast_float.git
    cd fast_float
    mkdir build && cd build
    cmake ..
    make -j"$(nproc)"
    make install
    cd ../../
fi

# Installing gengetopt
if ! [ -d "gengetopt-2.23" ]; then
    # Source the download retry function
    source "${BENCHPRESS_ROOT}/scripts/download_with_retry.sh"
    download_with_retry "https://mirrors.ocf.berkeley.edu/gnu/gengetopt/gengetopt-2.23.tar.xz"
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
    make -j"$(nproc)"
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
    make -j"$(nproc)"
    make install
    cd ../../
else
    msg "[SKIPPED] glog-0.4.0"
fi

# Update linker cache so shared libs (gflags, glog) are found at runtime
ldconfig

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
if ! [ -d "libevent-2.1.12-stable" ]; then
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

# Installing LibTorch for DLRM support
msg "Installing LibTorch for DLRM support..."
cd "${FEEDSIM_THIRD_PARTY_SRC}"

ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then
    LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-shared-with-deps-${LIBTORCH_VERSION}%2Bcpu.zip"
else
    die "Unsupported architecture: ${ARCH}"
fi

if ! [ -d "libtorch" ]; then
    if [ "${LIBTORCH_FROM_WHEEL}" = "1" ]; then
        # Extract LibTorch from the prebuilt torch CPU wheel. The wheel's
        # torch/ dir has the same lib/ include/ share/cmake/Torch/ layout as
        # the standalone libtorch zip, so we just rename it to libtorch/.
        msg "Downloading LibTorch ${LIBTORCH_VERSION} from torch CPU wheel..."
        # The C++ libtorch inside (torch/lib, torch/share/cmake) is Python-
        # version independent, so the cp310 wheel is fine for our C++ link.
        WHEEL_HREF="$(curl -s "https://download.pytorch.org/whl/cpu/torch/" \
            | grep -oE "https://[^\"]*torch-${LIBTORCH_VERSION}[^\"]*cp310-cp310-manylinux_2_28_x86_64\.whl" \
            | head -1)"
        [ -n "${WHEEL_HREF}" ] || die "Could not find torch ${LIBTORCH_VERSION} x86_64 wheel in index"
        msg "Wheel: ${WHEEL_HREF}"
        wget "${WHEEL_HREF}" -O torch.whl
        unzip -q torch.whl -d ./_torch_whl_x
        mv ./_torch_whl_x/torch libtorch
        rm -rf ./_torch_whl_x torch.whl
        msg "LibTorch ${LIBTORCH_VERSION} extracted from wheel to ${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
    else
        msg "Downloading LibTorch ${LIBTORCH_VERSION}..."
        wget "${LIBTORCH_URL}" -O libtorch.zip
        msg "Extracting LibTorch..."
        unzip -q libtorch.zip
        rm libtorch.zip
        msg "LibTorch installed to ${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
    fi
else
    msg "[SKIPPED] LibTorch already installed"
fi

# Download DLRM model
msg "Downloading DLRM model..."
DLRM_MODEL_DIR="${FEEDSIM_ROOT_SRC}/models"
mkdir -p "${DLRM_MODEL_DIR}"

if ! [ -f "${DLRM_MODEL_DIR}/dlrm_small.pt" ]; then
    msg "Downloading DLRM model from ${DLRM_MODEL_URL}..."
    wget "${DLRM_MODEL_URL}" -O "${DLRM_MODEL_DIR}/dlrm_small.tar.gz"
    msg "Extracting DLRM model..."
    tar -xzf "${DLRM_MODEL_DIR}/dlrm_small.tar.gz" -C "${DLRM_MODEL_DIR}"
    rm "${DLRM_MODEL_DIR}/dlrm_small.tar.gz"
    msg "DLRM model installed to ${DLRM_MODEL_DIR}"
else
    msg "[SKIPPED] DLRM model already installed"
fi


# Installing FeedSim
cd "${FEEDSIM_ROOT_SRC}"

cd "src"

# Populate third party submodules
while read -r submod;
do
    REPO="$(echo "$submod" | cut -d ' ' -f 1)"
    COMMIT="$(echo "$submod" | cut -d ' ' -f 2)"
    SUBMOD_DIR="$(echo "$submod" | cut -d ' ' -f 3)"
    if ! [ -d "${SUBMOD_DIR}" ]; then
        mkdir -p "${SUBMOD_DIR}"
        git clone "${REPO}" "${SUBMOD_DIR}"
        pushd "${SUBMOD_DIR}"
        git checkout "${COMMIT}"
        popd
    else
        msg "[SKIPPED] ${SUBMOD_DIR}"
    fi
done < "${FEEDSIM_ROOT}/submodules.txt"

# Patch fizz for OpenSSL 3.0 compatibility
if [ -f "third_party/fizz/fizz/tool/FizzServerCommand.cpp" ]; then
    # Replace EVP_PKEY_cmp with EVP_PKEY_eq
    sed -i 's/EVP_PKEY_cmp(pubKey.get(), key.get()) == 1/EVP_PKEY_eq(pubKey.get(), key.get())/g' "third_party/fizz/fizz/tool/FizzServerCommand.cpp"
fi

# Generate feature extractor variants (1M+ unique functions for I-cache pressure)
msg "Generating feature extractor variants..."
CODEGEN_DIR="${FEEDSIM_ROOT_SRC}/src/workloads/ranking/feature_extractors/generated"
if [ -f "${CODEGEN_DIR}/generate_extractors.py" ]; then
    python3 "${CODEGEN_DIR}/generate_extractors.py" --output-dir "${CODEGEN_DIR}"
    msg "Feature extractor codegen complete"
else
    msg "[SKIPPED] No codegen script found at ${CODEGEN_DIR}/generate_extractors.py"
fi

mkdir -p build && cd build/

# Build FeedSim with DLRM support
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG }"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic"

BP_CC="${BP_CC:-gcc}"
BP_CXX="${BP_CXX:-g++}"

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    -DTorch_DIR="${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share/cmake/Torch" \
    -DCMAKE_PREFIX_PATH="${FEEDSIM_THIRD_PARTY_SRC}/libtorch" \
    ../

# Third-party deps (fmt, folly, fizz, wangle, mvfst, fbthrift) are built by this
# ninja step via ExternalProject_Add. Their build order is declared in
# third_party/src/CMake/build-*.cmake via add_dependencies() and
# ExternalProject_Add_StepDependencies(), so ninja respects the DAG under -jN.
# Use nproc/2 to avoid OOM during heavy template-instantiation steps.
NINJA_JOBS="${BP_NINJA_JOBS:-$(( $(nproc) / 2 ))}"
[ "$NINJA_JOBS" -lt 1 ] && NINJA_JOBS=1
msg "Building FeedSim with ninja -j${NINJA_JOBS} (set BP_NINJA_JOBS to override)"
ninja -j"${NINJA_JOBS}"

msg ""
msg "=== FeedSim Installation Complete ==="
msg ""
msg "To run FeedSim with DLRM workload:"
msg "  cd ${FEEDSIM_ROOT_SRC}"
msg "  ./run.sh --workload=dlrm --dlrm-model=${DLRM_MODEL_DIR}/dlrm_small.pt"
msg ""
msg "Or use the standard PageRank workload:"
msg "  ./run.sh"
msg ""
