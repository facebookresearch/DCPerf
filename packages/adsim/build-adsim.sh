#!/bin/bash
# shellcheck disable=SC1091,SC2086,SC2164
# AdSim build script - compiles the ad simulation server with compression benchmarks

# Cross-platform package installation function for atomic library
install_atomic_library() {
    # Detect OS distribution and install atomic library with correct package name
    if command -v dnf >/dev/null 2>&1; then
        # Red Hat/Fedora/CentOS systems
        dnf install -y libatomic
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu/Debian systems
        apt-get update
        apt-get install -y libatomic1
    else
        echo "Error: No supported package manager found (dnf, and apt-get)"
        exit 1
    fi
}

# Load build configuration and environment variables
source config.sh

# Set compression library version for benchmarking workloads
LZBENCH_VERSION=v1.8.1

# Install atomic operations library required for multi-threaded performance
install_atomic_library

# Navigate to AdSim source directory and apply compatibility patches
quiet_pushd "${ADSIM_MAIN_SRC_DIR}"
patch -p1 --forward < ${ADSIM_PROJ_ROOT}/patches/adsim.patch

# Clean and recreate build directories
rm -rf build third_party
mkdir -p build third_party

# Copy CMake configuration and clone compression benchmark library
cp ${ADSIM_PROJ_ROOT}/buildfiles/adsim/cmake.txt build
git clone https://github.com/inikep/lzbench.git -b $LZBENCH_VERSION third_party/lzbench

# Apply patches to lzbench for AdSim integration
pushd third_party/lzbench || exit 1
patch -p1 --forward < ${ADSIM_PROJ_ROOT}/patches/lzbench.patch
popd

# Execute CMake build process
pushd build || exit 1
source cmake.txt
popd
