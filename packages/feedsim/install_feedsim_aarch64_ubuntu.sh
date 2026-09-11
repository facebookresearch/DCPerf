#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail
# trap cleanup SIGINT SIGTERM ERR EXIT

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

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
LIBTORCH_VERSION="${LIBTORCH_VERSION:-2.13.0}"
DLRM_MODEL_URL="https://github.com/facebookresearch/DCPerf-datasets/releases/download/feedsim-dlrm/dlrm_small.tar.gz"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

apt install -y bc cmake ninja-build flex bison texinfo binutils-dev \
    libunwind-dev bzip2 libbz2-dev libsodium-dev libghc-double-conversion-dev \
    libzstd-dev lz4 liblz4-dev xzip libsnappy-dev libtool libssl-dev \
    zlib1g-dev libdwarf-dev libaio-dev libatomic1 patch perl libiberty-dev \
    sysstat jq unzip xxhash libxxhash-dev libboost-all-dev rsync curl

# Install liburing >= 2.6 from source. Ubuntu's apt-shipped liburing is
# older than folly's minimum, so folly's io_uring integration links
# `-luring` against a nonexistent library. Build tag liburing-2.12 upstream
# and install into /usr/local so folly's `find_library` picks it up.
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

# Copy production size distribution JSONs (consumed by run.sh / DriverNodeRank).
cp "${BENCHPRESS_ROOT}/packages/feedsim/feed_aggregator_req_sizes.json" "${FEEDSIM_ROOT_SRC}/feed_aggregator_req_sizes.json"
cp "${BENCHPRESS_ROOT}/packages/feedsim/feed_aggregator_resp_sizes.json" "${FEEDSIM_ROOT_SRC}/feed_aggregator_resp_sizes.json"
# Phase 6 session-mode driver loads rpc_dist.json from the FEEDSIM_ROOT runtime dir.
cp "${BENCHPRESS_ROOT}/packages/feedsim/rpc_dist.json" "${FEEDSIM_ROOT_SRC}/rpc_dist.json"

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

# Installing fast_float
if ! [ -d "fast_float" ]; then
    git clone https://github.com/fastfloat/fast_float.git
    cd fast_float
    mkdir build && cd build
    cmake ..
    make
    make install
    cd ../../
fi

# Installing gengetopt
if ! [ -d "gengetopt-2.23" ]; then
    wget "https://mirrors.ocf.berkeley.edu/gnu/gengetopt/gengetopt-2.23.tar.xz"
    tar -xf "gengetopt-2.23.tar.xz"
    cd "gengetopt-2.23"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] gengetopt-2.23"
fi

# Using system Boost (installed via libboost-all-dev)
# This avoids ABI mismatch issues that occur when building Boost from source
msg "[INFO] Using system Boost (libboost-all-dev)"

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

# Update linker cache so shared libs (gflags, glog) are found at runtime
ldconfig

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

# Installing LibTorch via pip for aarch64
# PyTorch does not provide official pre-built LibTorch C++ binaries for ARM64
# Linux via conda or download.pytorch.org/libtorch. The conda default channel
# now ships CUDA-enabled libtorch (gpu_cuda130) even on aarch64, which fails
# on machines without CUDA.
# Instead, we install the CPU-only torch wheel via pip and extract the
# libtorch cmake/headers/libs from the pip package.
msg "Installing LibTorch via pip (CPU-only) for aarch64..."
cd "${FEEDSIM_THIRD_PARTY_SRC}"

MINICONDA_VERSION="latest"
CONDA_DIR="${FEEDSIM_THIRD_PARTY_SRC}/miniconda3"

if ! [ -d "libtorch" ]; then
    # Install Miniconda for a clean Python environment
    from_user_conda=0
    if ! [ -d "${CONDA_DIR}" ]; then
        if ! command -v conda >/dev/null 2>&1; then
            msg "Installing Miniconda..."
            ARCH="$(uname -m)"
            MINICONDA_URL="https://repo.anaconda.com/miniconda/Miniconda3-${MINICONDA_VERSION}-Linux-${ARCH}.sh"
            wget "${MINICONDA_URL}" -O miniconda.sh
            bash miniconda.sh -b -p "${CONDA_DIR}"
            rm miniconda.sh
        else
            from_user_conda=1
            msg "Installing benchmark-local conda from user's conda"
            conda create -p "${CONDA_DIR}" -y -c conda-forge conda
        fi
    fi

    export PATH="${CONDA_DIR}/bin:${PATH}"

    # Install CPU-only PyTorch via pip — this is the only reliable way to get
    # CPU-only libtorch on aarch64. LIBTORCH_VERSION (env) pins the version;
    # empty falls back to pip's latest resolution.
    msg "Installing PyTorch CPU-only via pip (version='${LIBTORCH_VERSION:-latest}')..."
    if [ -n "${LIBTORCH_VERSION:-}" ]; then
        pip install "torch==${LIBTORCH_VERSION}+cpu" --index-url https://download.pytorch.org/whl/cpu
    else
        pip install torch --index-url https://download.pytorch.org/whl/cpu
    fi

    # Also install libstdcxx-ng to ensure compatible C++ runtime
    eval "$("${CONDA_DIR}/bin/conda" shell.bash hook)"
    if [ "${from_user_conda}" = 0 ]; then
        conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main || true
    fi
    conda install -y -c conda-forge libstdcxx-ng

    # Locate the pip-installed torch package
    TORCH_DIR="$(${CONDA_DIR}/bin/python3 -c 'import torch, os; print(os.path.dirname(torch.__file__))')"
    msg "Found torch at: ${TORCH_DIR}"

    # Create libtorch directory structure with symlinks to pip torch
    mkdir -p "${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
    ln -sf "${TORCH_DIR}/include" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/include"
    ln -sf "${TORCH_DIR}/lib" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/lib"
    ln -sf "${TORCH_DIR}/share" "${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share"

    msg "LibTorch (CPU-only) installed via pip: ${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
else
    msg "[SKIPPED] LibTorch already installed"
fi

# Always remove conda cmake files that reference CUDA — they confuse find_package
# even when libtorch symlinks point to the CPU-only pip torch
rm -rf "${CONDA_DIR}/share/cmake/Caffe2" "${CONDA_DIR}/share/cmake/Torch" 2>/dev/null || true

# Set up environment to use conda's libstdc++ for compatibility with libtorch
# This resolves GLIBCXX version mismatch between system and conda libraries
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

# Download Silesia compression corpus for story-based requests (Phase 3)
SILESIA_DIR="${FEEDSIM_ROOT_SRC}/silesia"
SILESIA_URL="https://github.com/facebookresearch/DCPerf-datasets/releases/download/feedsim-silesia/silesia.tar.gz"
if ! [ -d "$SILESIA_DIR" ] || [ -z "$(ls -A "$SILESIA_DIR" 2>/dev/null)" ]; then
    msg "Downloading Silesia corpus..."
    mkdir -p "$SILESIA_DIR"
    cd "$SILESIA_DIR" || { msg "[ERROR] cannot cd to $SILESIA_DIR"; exit 1; }
    if wget -q "$SILESIA_URL" -O silesia.tar.gz 2>/dev/null; then
        tar -xzf silesia.tar.gz && rm -f silesia.tar.gz
        msg "Silesia corpus downloaded: $(ls | wc -l) files, $(du -sh . | cut -f1)"
    else
        msg "[INFO] GitHub dataset not available, trying original Silesia host..."
        SILESIA_FALLBACK_URL="https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
        if wget -q "$SILESIA_FALLBACK_URL" -O silesia.zip 2>/dev/null; then
            unzip -q silesia.zip && rm -f silesia.zip
            msg "Silesia corpus downloaded: $(ls | wc -l) files, $(du -sh . | cut -f1)"
        else
            msg "[WARNING] Silesia download failed — story-based requests will be unavailable"
        fi
    fi
    cd "${FEEDSIM_THIRD_PARTY_SRC}"
else
    msg "[SKIPPED] Silesia corpus already present at $SILESIA_DIR"
fi

# Extract example TLS certs for mock_services (used when --tls_cert/--tls_key
# are passed; see run-feedsim-multi.sh). The tarball ships example.crt and
# example.key suitable for benchmark use only (no peer verification).
CERTS_DIR="${FEEDSIM_ROOT_SRC}/certs"
CERTS_TARBALL="${BENCHPRESS_ROOT}/packages/common/certs.tar.gz"
if [ -f "$CERTS_TARBALL" ]; then
    mkdir -p "$CERTS_DIR"
    # --strip-components=1 drops the top-level `certs/` directory inside the
    # tarball so the files land directly at $CERTS_DIR/example.{crt,key}.
    tar -xzf "$CERTS_TARBALL" -C "$CERTS_DIR" --strip-components=1
    msg "Extracted TLS certs to $CERTS_DIR"
else
    msg "[WARNING] $CERTS_TARBALL not found; TLS for mock_services will be unavailable"
fi


# Installing FeedSim
cd "${FEEDSIM_ROOT_SRC}"

cd "src"

# Populate third party submodules
msg "Checking out submodules..."
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

# Remove liburing-dev — Ubuntu's version is too old for folly v2026.01.05.00
# which uses io_uring zero-copy RX APIs requiring liburing >= 2.6
apt remove -y liburing-dev 2>/dev/null || true

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
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic -L${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib -Wl,-rpath,${FEEDSIM_THIRD_PARTY_SRC}/miniconda3/lib"

BP_CC=gcc
BP_CXX=g++

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${FEEDSIM_THIRD_PARTY_SRC}/build-deps" \
    -DBOOST_ROOT="/usr" \
    -DBoost_INCLUDE_DIR="/usr/include" \
    -DBoost_LIBRARY_DIR="/usr/lib/aarch64-linux-gnu" \
    -DBoost_NO_SYSTEM_PATHS=OFF \
    -DBoost_NO_BOOST_CMAKE=ON \
    -DCMAKE_C_COMPILER="$BP_CC" \
    -DCMAKE_CXX_COMPILER="$BP_CXX" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    -DTorch_DIR="${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share/cmake/Torch" \
    ../

ninja -v -j1

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
