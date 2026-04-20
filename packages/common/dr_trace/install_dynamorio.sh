#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# install_dynamorio.sh: Build DynamoRIO from source
#
# This script is meant to be sourced by benchmark install scripts.
# It clones DynamoRIO at a pinned version, builds it, and installs
# it to $BUILD_DIR/dynamorio.
#
# Prerequisites:
#   - BUILD_DIR must be set to the benchmark's build directory
#   - cmake, git, and a C/C++ compiler must be available
#   - sudo access (to install system dependencies)
#
# After sourcing:
#   - DR_INSTALL is exported, pointing to the DynamoRIO install prefix
#   - DynamoRIO binaries (drraw2trace, drrun, etc.) are at $DR_INSTALL/tools/bin64/

# ---- Prerequisite checks ----

fail() {
  echo "[DR_TRACE] ERROR: $1" >&2
  return 1 2>/dev/null || exit 1
}

if [ -z "${BUILD_DIR}" ]; then
  fail "BUILD_DIR is not set. Set it to the benchmark's build directory."
fi

if [ ! -d "${BUILD_DIR}" ]; then
  fail "BUILD_DIR='${BUILD_DIR}' does not exist."
fi

for cmd in cmake git; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    fail "'${cmd}' is not installed or not in PATH."
  fi
done

# Check for a working C++ compiler (cmake will fail late with a cryptic error)
if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
  fail "No C++ compiler found (need c++, g++, or clang++ in PATH)."
fi

# ---- Build configuration ----

# Pinned DynamoRIO version (cronbuild tag from GitHub)
DR_VERSION="11.90.20482"
DR_TAG="cronbuild-${DR_VERSION}"

DR_SRC="${BUILD_DIR}/dynamorio_src"
DR_INSTALL="${BUILD_DIR}/dynamorio"

# Skip if already built
if [ -f "${DR_INSTALL}/cmake/DynamoRIOConfig.cmake" ]; then
  echo "[DR_TRACE] DynamoRIO already installed at: ${DR_INSTALL}"
  export DR_INSTALL
  export DynamoRIO_DIR="${DR_INSTALL}/cmake"
  # shellcheck disable=SC2317  # exit 0 is reachable when script is executed (not sourced)
  return 0 2>/dev/null || exit 0
fi

echo "[DR_TRACE] Building DynamoRIO ${DR_VERSION} from source..."

# Install system dependencies for DynamoRIO and dr_trace
echo "[DR_TRACE] Installing system dependencies..."
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get install -y -qq libelf-dev zlib1g-dev liblz4-dev libsnappy-dev
elif command -v dnf >/dev/null 2>&1; then
  sudo dnf install -y -q elfutils-libelf-devel zlib-devel lz4-devel snappy-devel
fi

# Clone with submodules (elfutils is a submodule required by drsyms)
if [ ! -d "${DR_SRC}" ]; then
  echo "[DR_TRACE] Cloning DynamoRIO..."
  git clone --depth 1 --recursive --branch "${DR_TAG}" \
    https://github.com/DynamoRIO/dynamorio.git "${DR_SRC}"
fi

# Build
echo "[DR_TRACE] Configuring DynamoRIO..."
mkdir -p "${DR_SRC}/build"
cmake -S "${DR_SRC}" -B "${DR_SRC}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${DR_INSTALL}" \
  -DBUILD_SAMPLES=OFF \
  -DBUILD_TESTS=OFF \
  -DBUILD_DOCS=OFF

echo "[DR_TRACE] Compiling DynamoRIO (this may take several minutes)..."
cmake --build "${DR_SRC}/build" --parallel "$(nproc)"

echo "[DR_TRACE] Installing DynamoRIO..."
cmake --install "${DR_SRC}/build"

# Export for CMake
export DR_INSTALL
export DynamoRIO_DIR="${DR_INSTALL}/cmake"

echo "[DR_TRACE] DynamoRIO installed at: ${DR_INSTALL}"
echo "[DR_TRACE] DR_INSTALL=${DR_INSTALL}"
