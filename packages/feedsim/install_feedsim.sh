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
# Dependency versions are env-overridable so experiments can bump them without
# forking this script; defaults reproduce the v2 baseline exactly.
JEMALLOC_VERSION="${FEEDSIM_JEMALLOC_VERSION:-5.3.0}"
LIBEVENT_VERSION="${FEEDSIM_LIBEVENT_VERSION:-2.1.12-stable}"
# Export so the aarch64 sub-installer (dispatched below) inherits the pins.
export LIBTORCH_VERSION LIBTORCH_FROM_WHEEL FEEDSIM_JEMALLOC_VERSION FEEDSIM_LIBEVENT_VERSION
DLRM_MODEL_URL="https://github.com/facebookresearch/DCPerf-datasets/releases/download/feedsim-dlrm/dlrm_small.tar.gz"
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

source "${BENCHPRESS_ROOT}/packages/common/os-distro.sh"

msg() {
  echo >&2 -e "${1-}"
}

die() {
  local msg=$1
  local code=${2-1} # default exit status 1
  msg "$msg"
  exit "$code"
}

ARCH="$(uname -m)"
if [ "$ARCH" = "aarch64" ]; then
  if distro_is_like ubuntu; then
    "${FEEDSIM_ROOT}"/install_feedsim_aarch64_ubuntu.sh
     exit $?
  else
    "${FEEDSIM_ROOT}"/install_feedsim_aarch64.sh
    exit $?
  fi
fi
if distro_is_like ubuntu && [ "$(uname -m)" = "x86_64" ]; then
  "${FEEDSIM_ROOT}"/install_feedsim_ubuntu.sh
  exit $?
fi
dnf install -y bc ninja-build flex bison git texinfo binutils-devel \
    libsodium-devel libunwind-devel bzip2-devel double-conversion-devel \
    libzstd-devel lz4-devel xz-devel snappy-devel libtool bzip2 openssl-devel \
    zlib-devel libdwarf libdwarf-devel libaio-devel libatomic patch jq \
    xxhash xxhash-devel unzip rsync liburing-devel python3-pip

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
# Sync feedsim source code with --delete so re-install (`./benchpress install -f`)
# actually picks up source changes. The previous `cp -r ... && mv` pattern silently
# nested into an existing src/ tree on re-install (mv into a non-empty target moves
# INTO it instead of overwriting), leaving stale source files. rsync with trailing
# slashes on both src and dst does file-level overwrite + deletion of removed files.
#
# CRITICAL --exclude=third_party: subsequent install steps git-clone submodules
# (cereal, fbthrift, folly, wangle, fizz, mvfst, ...) INTO ${FEEDSIM_ROOT_SRC}/src/
# third_party/<submod>/. Without this exclude, rsync --delete wipes those submodule
# directories on every re-install, forcing a re-clone of ~GB of code. The exclude
# preserves them. The top-level src/third_party/CMakeLists.txt (the only file
# packages/feedsim/third_party/src/third_party/ actually owns) is copied separately
# below.
mkdir -p "${FEEDSIM_ROOT_SRC}/src" "${FEEDSIM_THIRD_PARTY_SRC}"
rsync -a --delete --exclude=third_party \
    "${BENCHPRESS_ROOT}/packages/feedsim/third_party/src/" \
    "${FEEDSIM_ROOT_SRC}/src/"
mkdir -p "${FEEDSIM_ROOT_SRC}/src/third_party"
cp -f "${BENCHPRESS_ROOT}/packages/feedsim/third_party/src/third_party/CMakeLists.txt" \
    "${FEEDSIM_ROOT_SRC}/src/third_party/CMakeLists.txt"
# Sync the rest of third_party/ (cmake source dir, fbthrift submodule, etc.) WITHOUT
# --delete because subsequent steps download cmake / boost / libtorch into this dir
# and we don't want to wipe them on re-install. The --exclude=src keeps src/ out of
# this rsync — it's already handled above and lives under ${FEEDSIM_ROOT_SRC}/src.
rsync -a --exclude=src \
    "${BENCHPRESS_ROOT}/packages/feedsim/third_party/" \
    "${FEEDSIM_THIRD_PARTY_SRC}/"
cd "${FEEDSIM_THIRD_PARTY_SRC}"

# Optionally build DynamoRIO for tracing support.
DR_TRACE_FLAGS=()
if [ "${ENABLE_DR_TRACE:-0}" = "1" ]; then
  msg "[DR_TRACE] Setting up DynamoRIO tracing support..."
  BUILD_DIR="${FEEDSIM_THIRD_PARTY_SRC}"
  export BUILD_DIR
  # shellcheck disable=SC1091
  source "${BENCHPRESS_ROOT}/packages/common/dr_trace/install_dynamorio.sh"
  DR_TRACE_FLAGS=(
    -DENABLE_DR_TRACE=ON
    -DDR_INSTALL="${DR_INSTALL}"
    -DDR_TRACE_DIR="${BENCHPRESS_ROOT}/packages/common/dr_trace"
  )
  msg "[DR_TRACE] DR_INSTALL=${DR_INSTALL}"
fi

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
    make
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
if ! [ -d "jemalloc-${JEMALLOC_VERSION}" ]; then
    wget "https://github.com/jemalloc/jemalloc/releases/download/${JEMALLOC_VERSION}/jemalloc-${JEMALLOC_VERSION}.tar.bz2"
    bunzip2 "jemalloc-${JEMALLOC_VERSION}.tar.bz2"
    tar -xvf "jemalloc-${JEMALLOC_VERSION}.tar"
    cd "jemalloc-${JEMALLOC_VERSION}"
    ./configure --enable-prof --enable-prof-libunwind
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] jemalloc-${JEMALLOC_VERSION}"
fi

# Installing libevent
if ! [ -d "libevent-${LIBEVENT_VERSION}" ]; then
    wget "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz"
    tar -xzf "libevent-${LIBEVENT_VERSION}.tar.gz"
    cd "libevent-${LIBEVENT_VERSION}"
    ./configure
    make -j"$(nproc)"
    make install
    cd ../
else
    msg "[SKIPPED] libevent-${LIBEVENT_VERSION}"
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
        # pip on the box (3.9, or an internal stale mirror) can't see the cp310
        # 2.13 wheels, so resolve the wheel href straight from the PEP-503 index
        # and wget it. The C++ libtorch inside (torch/lib, torch/share/cmake) is
        # Python-version independent, so the cp310 wheel is fine for our C++ link.
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
        # Fallback to original Silesia host
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
# -luring: folly's IoUringZeroCopyBufferPool/IoUringEvent reference io_uring
# symbols but folly's CMake doesn't propagate liburing as a transitive link
# dependency. On RHEL9 + liburing 2.12 the static libfolly.a otherwise fails
# to link LeafNodeRank with undefined references to io_uring_register_ifq /
# io_uring_register_eventfd. Force the link explicitly.
FS_LDFLAGS="${BP_LDFLAGS:-} -luring -latomic -Wl,--export-dynamic"

cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${BP_CC:-gcc}" \
    -DCMAKE_CXX_COMPILER="${BP_CXX:-g++}" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    -DFEEDSIM_USE_DLRM=ON \
    -DTorch_DIR="${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share/cmake/Torch" \
    -DCMAKE_PREFIX_PATH="${FEEDSIM_THIRD_PARTY_SRC}/libtorch" \
    "${DR_TRACE_FLAGS[@]}" \
    ../

# Dependencies (fmt, folly, fbthrift, etc.) are already installed above via
# separate make commands in their own build directories. This ninja step only
# builds FeedSim itself (LeafNodeRank, DriverNodeRank, feature extractors),
# so parallel builds are safe here. Use nproc/2 to avoid OOM.
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
