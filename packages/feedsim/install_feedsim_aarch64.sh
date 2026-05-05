#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates and Contributors
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

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

verify_checksum() {
    local file="$1"
    local expected_checksum="$2"

    if ! [ -f "$file" ]; then
        echo "WARNING: File not found: $file"
        exit 1
    fi

    local actual_checksum
    actual_checksum=$(sha256sum "$file" | awk '{print $1}')

    if [[ "$actual_checksum" != "$expected_checksum" ]]; then
        echo "WARNING: Checksum mismatch for file: $file"
        exit 1
    fi
}

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
DLRM_MODEL_URL="https://github.com/facebookresearch/DCPerf-datasets/releases/download/feedsim-dlrm/dlrm_small.tar.gz"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

dnf install -y bc ninja-build flex bison git texinfo binutils-devel \
    libsodium-devel libunwind-devel bzip2-devel double-conversion-devel \
    libzstd-devel lz4-devel xz-devel snappy-devel libtool bzip2 openssl-devel \
    zlib-devel libdwarf libdwarf-devel libaio-devel libatomic patch jq \
    xxhash xxhash-devel unzip

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

DEP_CMAKE_VERSION="4.0.3"
# Installing cmake
if ! [ -d "cmake-${DEP_CMAKE_VERSION}-linux-aarch64" ]; then
    wget "https://github.com/Kitware/CMake/releases/download/v${DEP_CMAKE_VERSION}/cmake-${DEP_CMAKE_VERSION}-linux-aarch64.tar.gz" -O "cmake-${DEP_CMAKE_VERSION}-linux-aarch64.tar.gz"
    verify_checksum "cmake-${DEP_CMAKE_VERSION}-linux-aarch64.tar.gz" "391da1544ef50ac31300841caaf11db4de3976cdc4468643272e44b3f4644713"
    tar xfz "cmake-${DEP_CMAKE_VERSION}-linux-aarch64.tar.gz"
    export PATH="${FEEDSIM_THIRD_PARTY_SRC}/cmake-${DEP_CMAKE_VERSION}-linux-aarch64/bin:${PATH}"
else
    msg "[SKIPPED] cmake-${DEP_CMAKE_VERSION}"
fi

# Installing fast_float
if ! [ -d "fast_float" ]; then
    git clone https://github.com/fastfloat/fast_float.git
    cd fast_float
    git checkout v8.1.0
    mkdir build && cd build
    cmake ..
    make -j"$(nproc)"
    make install
    cd ../../
else
    msg "[SKIPPED] fast_float"
fi

# Installing gengetopt
DEP_GENGOPT_VERSION="2.23"
if ! [ -d "gengetopt-${DEP_GENGOPT_VERSION}" ]; then
    # Source the download retry function
    source "${BENCHPRESS_ROOT}/scripts/download_with_retry.sh"
    download_with_retry "https://mirrors.ocf.berkeley.edu/gnu/gengetopt/gengetopt-${DEP_GENGOPT_VERSION}.tar.xz"
    verify_checksum "gengetopt-${DEP_GENGOPT_VERSION}.tar.xz" "b941aec9011864978dd7fdeb052b1943535824169d2aa2b0e7eae9ab807584ac"
    tar -xf "gengetopt-${DEP_GENGOPT_VERSION}.tar.xz"
    cd "gengetopt-${DEP_GENGOPT_VERSION}"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] gengetopt-${DEP_GENGOPT_VERSION}"
fi

DEP_BOOST_VERSION="1_88_0"
# Installing Boost
if ! [ -d "boost_${DEP_BOOST_VERSION}" ]; then
    wget "https://archives.boost.io/release/$(echo $DEP_BOOST_VERSION | sed 's/_/./g')/source/boost_${DEP_BOOST_VERSION}.tar.gz" -O "boost_${DEP_BOOST_VERSION}.tar.gz"
    verify_checksum "boost_${DEP_BOOST_VERSION}.tar.gz" "3621533e820dcab1e8012afd583c0c73cf0f77694952b81352bf38c1488f9cb4"
    tar -xzf "boost_${DEP_BOOST_VERSION}.tar.gz"
    cd "boost_${DEP_BOOST_VERSION}"
    ./bootstrap.sh --without-libraries=python
    ./b2 -j"$(nproc)" install
    cd ../
else
    msg "[SKIPPED] boost_${DEP_BOOST_VERSION}"
fi

DEP_GFLAGS_VERSION="2.2.2"
# Installing gflags
if ! [ -d "gflags-${DEP_GFLAGS_VERSION}" ]; then
    wget "https://github.com/gflags/gflags/archive/refs/tags/v${DEP_GFLAGS_VERSION}.tar.gz" -O "gflags-${DEP_GFLAGS_VERSION}.tar.gz"
    verify_checksum "gflags-${DEP_GFLAGS_VERSION}.tar.gz" "34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf"
    tar -xzf "gflags-${DEP_GFLAGS_VERSION}.tar.gz"
    cd "gflags-${DEP_GFLAGS_VERSION}"
    mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ../
    make -j"$(nproc)"
    make install
    cd ../../
else
    msg "[SKIPPED] gflags-${DEP_GFLAGS_VERSION}"
fi

DEP_GFLAGS_VERSION="0.4.0"
# Installing glog
if ! [ -d "glog-${DEP_GFLAGS_VERSION}" ]; then
    wget "https://github.com/google/glog/archive/refs/tags/v${DEP_GFLAGS_VERSION}.tar.gz" -O "glog-${DEP_GFLAGS_VERSION}.tar.gz"
    verify_checksum "glog-${DEP_GFLAGS_VERSION}.tar.gz" "f28359aeba12f30d73d9e4711ef356dc842886968112162bc73002645139c39c"
    tar -xzf "glog-${DEP_GFLAGS_VERSION}.tar.gz"
    cd "glog-${DEP_GFLAGS_VERSION}"
    mkdir -p build && cd build
    cmake -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ../
    make -j"$(nproc)"
    make install
    cd ../../
else
    msg "[SKIPPED] glog-${DEP_GFLAGS_VERSION}"
fi

DEP_JEMALLOC_VERSION="5.3.0"
# Installing JEMalloc
if ! [ -d "jemalloc-${DEP_JEMALLOC_VERSION}" ]; then
    wget "https://github.com/jemalloc/jemalloc/releases/download/${DEP_JEMALLOC_VERSION}/jemalloc-${DEP_JEMALLOC_VERSION}.tar.bz2" -O "jemalloc-${DEP_JEMALLOC_VERSION}.tar.bz2"
    verify_checksum "jemalloc-${DEP_JEMALLOC_VERSION}.tar.bz2" "2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa"
    bunzip2 "jemalloc-${DEP_JEMALLOC_VERSION}.tar.bz2"
    tar -xvf "jemalloc-${DEP_JEMALLOC_VERSION}.tar"
    cd "jemalloc-${DEP_JEMALLOC_VERSION}"
    ./configure --enable-prof --enable-prof-libunwind
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] jemalloc-${DEP_JEMALLOC_VERSION}"
fi

DEP_LIBEVENT_VERSION="2.1.12-stable"
# Installing libevent
if ! [ -d "libevent-${DEP_LIBEVENT_VERSION}" ]; then
    wget "https://github.com/libevent/libevent/releases/download/release-${DEP_LIBEVENT_VERSION}/libevent-${DEP_LIBEVENT_VERSION}.tar.gz" -O "libevent-${DEP_LIBEVENT_VERSION}.tar.gz"
    verify_checksum "libevent-${DEP_LIBEVENT_VERSION}.tar.gz" "92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"
    tar -xzf "libevent-${DEP_LIBEVENT_VERSION}.tar.gz"
    cd "libevent-${DEP_LIBEVENT_VERSION}"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] libevent-${DEP_LIBEVENT_VERSION}"
fi

msg "Installing third-party dependencies ... DONE"

# Installing LibTorch via pip for aarch64
# PyTorch does not provide official pre-built LibTorch C++ binaries for ARM64
# Linux via conda or download.pytorch.org/libtorch. The conda default channel
# now ships CUDA-enabled libtorch (gpu_cuda130) even on aarch64, which fails
# on machines without CUDA (e.g., Grace).
# Instead, we install the CPU-only torch wheel via pip and extract the
# libtorch cmake/headers/libs from the pip package.
msg "Installing LibTorch via pip (CPU-only) for aarch64..."
cd "${FEEDSIM_THIRD_PARTY_SRC}"

MINICONDA_VERSION="latest"
CONDA_DIR="${FEEDSIM_THIRD_PARTY_SRC}/miniconda3"

if ! [ -d "libtorch" ]; then
    # Install Miniconda for a clean Python environment
    if ! [ -d "${CONDA_DIR}" ]; then
        msg "Installing Miniconda..."
        ARCH="$(uname -m)"
        MINICONDA_URL="https://repo.anaconda.com/miniconda/Miniconda3-${MINICONDA_VERSION}-Linux-${ARCH}.sh"
        wget "${MINICONDA_URL}" -O miniconda.sh
        bash miniconda.sh -b -p "${CONDA_DIR}"
        rm miniconda.sh
    fi

    export PATH="${CONDA_DIR}/bin:${PATH}"

    # Install CPU-only PyTorch via pip — this is the only reliable way to get
    # CPU-only libtorch on aarch64
    msg "Installing PyTorch CPU-only via pip..."
    pip install torch --index-url https://download.pytorch.org/whl/cpu

    # Also install libstdcxx-ng to ensure compatible C++ runtime
    eval "$("${CONDA_DIR}/bin/conda" shell.bash hook)"
    conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main || true
    conda install -y -c conda-forge libstdcxx-ng

    # Locate the pip-installed torch package
    TORCH_DIR="$(${CONDA_DIR}/bin/python3 -c 'import torch, os; print(os.path.dirname(torch.__file__))')"
    msg "Found torch at: ${TORCH_DIR}"

    # Create libtorch directory structure with symlinks to pip torch
    mkdir -p "${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
    ln -sf "${TORCH_DIR}/include" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/include"
    ln -sf "${TORCH_DIR}/lib" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/lib"
    ln -sf "${TORCH_DIR}/share" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share"

    # Remove any leftover conda cmake files that could confuse find_package
    rm -rf "${CONDA_DIR}/share/cmake/Caffe2" "${CONDA_DIR}/share/cmake/Torch"

    msg "LibTorch (CPU-only) installed via pip: ${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
else
    msg "[SKIPPED] LibTorch already installed"
fi

# Always remove conda cmake files that reference CUDA — they confuse find_package
# even when libtorch symlinks point to the CPU-only pip torch
rm -rf "${CONDA_DIR}/share/cmake/Caffe2" "${CONDA_DIR}/share/cmake/Torch" 2>/dev/null || true

# Set up environment to use conda's libstdc++ for compatibility with libtorch
export LD_LIBRARY_PATH="${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib:${LD_LIBRARY_PATH:-}"
export LIBRARY_PATH="${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib:${LIBRARY_PATH:-}"

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
cd "${FEEDSIM_ROOT_SRC}/src"

msg "Initializing third-party submodules"
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
msg "Initializing third-party submodules ... DONE"

# Patch fizz for OpenSSL 3.0 compatibility
if [ -f "third_party/fizz/fizz/tool/FizzServerCommand.cpp" ]; then
    # Replace EVP_PKEY_cmp with EVP_PKEY_eq
    sed -i 's/EVP_PKEY_cmp(pubKey.get(), key.get()) == 1/EVP_PKEY_eq(pubKey.get(), key.get())/g' "third_party/fizz/fizz/tool/FizzServerCommand.cpp"
fi

msg "Building FeedSim ..."
mkdir -p build && cd build/

# Build FeedSim
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG -Wno-deprecated-declarations}"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic -L${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib -Wl,-rpath,${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib"

BP_CC="${BP_CC:-gcc}"
BP_CXX="${BP_CXX:-g++}"

# Use system OpenSSL

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${FEEDSIM_THIRD_PARTY_SRC}/build-deps" \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    -DFEEDSIM_USE_DLRM=ON \
    -DTorch_DIR="${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share/cmake/Torch" \
    ../

ninja-build -j 1

msg ""
msg "=== FeedSim Installation Complete ==="
msg ""
msg "To run FeedSim with DLRM workload:"
msg "  cd ${FEEDSIM_ROOT_SRC}"
msg "  export LD_LIBRARY_PATH=${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib:\$LD_LIBRARY_PATH"
msg "  ./run.sh --workload=dlrm --dlrm-model=${DLRM_MODEL_DIR}/dlrm_small.pt"
msg ""
msg "Or use the standard PageRank workload:"
msg "  ./run.sh"
msg ""
