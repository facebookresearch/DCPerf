#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Install FeedSim with DLRM support
# This script wraps the standard install_feedsim.sh and adds LibTorch

set -Eeuo pipefail

# Constants
FEEDSIM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "$FEEDSIM_ROOT/../..")"
FEEDSIM_ROOT_SRC="${BENCHPRESS_ROOT}/benchmarks/feedsim"
FEEDSIM_THIRD_PARTY_SRC="${FEEDSIM_ROOT_SRC}/third_party"
LIBTORCH_VERSION="2.1.0"

echo "=== FeedSim DLRM Installation Script ==="
echo "BENCHPRESS_ROOT is ${BENCHPRESS_ROOT}"

msg() {
  echo >&2 -e "${1-}"
}

die() {
  local msg=$1
  local code=${2-1}
  msg "$msg"
  exit "$code"
}

# First run the standard installation
msg "Step 1: Running standard FeedSim installation..."
"${FEEDSIM_ROOT}/install_feedsim.sh"

# Now install LibTorch
msg ""
msg "Step 2: Installing LibTorch for DLRM support..."

cd "${FEEDSIM_THIRD_PARTY_SRC}"

ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then
    LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-${LIBTORCH_VERSION}%2Bcpu.zip"
elif [ "$ARCH" = "aarch64" ]; then
    # For ARM, we need to build from source or use a different approach
    msg "WARNING: Pre-built LibTorch for ARM64 may not be available."
    msg "Attempting to download CPU version..."
    LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-${LIBTORCH_VERSION}%2Bcpu.zip"
else
    die "Unsupported architecture: ${ARCH}"
fi

if ! [ -d "libtorch" ]; then
    msg "Downloading LibTorch ${LIBTORCH_VERSION}..."
    wget "${LIBTORCH_URL}" -O libtorch.zip
    msg "Extracting LibTorch..."
    unzip -q libtorch.zip
    rm libtorch.zip
    msg "LibTorch installed to ${FEEDSIM_THIRD_PARTY_SRC}/libtorch"
else
    msg "[SKIPPED] LibTorch already installed"
fi

# Copy the DLRM model if it exists
DLRM_MODEL_SRC="/home/wsu/feedsim_v2/models/dlrm_small.pt"
DLRM_MODEL_DST="${FEEDSIM_ROOT_SRC}/models/dlrm_small.pt"

if [ -f "$DLRM_MODEL_SRC" ]; then
    msg "Copying DLRM model..."
    mkdir -p "${FEEDSIM_ROOT_SRC}/models"
    cp "$DLRM_MODEL_SRC" "$DLRM_MODEL_DST"
    msg "DLRM model copied to ${DLRM_MODEL_DST}"
else
    msg "WARNING: DLRM model not found at ${DLRM_MODEL_SRC}"
    msg "You will need to provide the model path when running FeedSim with DLRM"
fi

# Rebuild FeedSim with DLRM support
msg ""
msg "Step 3: Rebuilding FeedSim with DLRM support..."

cd "${FEEDSIM_ROOT_SRC}/src"

# Remove old build to ensure clean rebuild with DLRM
if [ -d "build" ]; then
    msg "Removing old build directory..."
    rm -rf build
fi

mkdir -p build && cd build/

# Build FeedSim with DLRM enabled
FS_CFLAGS="${BP_CFLAGS:--O3 -DNDEBUG}"
FS_CXXFLAGS="${BP_CXXFLAGS:--O3 -DNDEBUG}"
FS_LDFLAGS="${BP_LDFLAGS:-} -latomic -Wl,--export-dynamic"

export PATH="${FEEDSIM_THIRD_PARTY_SRC}/cmake-4.0.3/staging/bin:${PATH}"

msg "Configuring with CMake (DLRM enabled)..."
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${BP_CC:-gcc}" \
    -DCMAKE_CXX_COMPILER="${BP_CXX:-g++}" \
    -DCMAKE_C_FLAGS_RELEASE="$FS_CFLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$FS_CXXFLAGS -DFMT_HEADER_ONLY=1" \
    -DCMAKE_EXE_LINKER_FLAGS_RELEASE="$FS_LDFLAGS" \
    -DFEEDSIM_USE_DLRM=ON \
    -DTorch_DIR="${FEEDSIM_THIRD_PARTY_SRC}/libtorch/share/cmake/Torch" \
    -DCMAKE_PREFIX_PATH="${FEEDSIM_THIRD_PARTY_SRC}/libtorch" \
    ../

msg "Building FeedSim with DLRM..."
ninja -v

msg ""
msg "=== FeedSim DLRM Installation Complete ==="
msg ""
msg "To run FeedSim with DLRM workload:"
msg "  cd ${FEEDSIM_ROOT_SRC}"
msg "  ./run.sh -W dlrm -M ${DLRM_MODEL_DST}"
msg ""
msg "Or use the standard PageRank workload:"
msg "  ./run.sh"
msg ""
