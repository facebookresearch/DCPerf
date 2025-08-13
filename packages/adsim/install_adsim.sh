#!/bin/bash
# shellcheck disable=SC2086
# AdSim installer - orchestrates build of ad simulation server and load testing tools

# Cross-platform package installation function
install_packages() {
    local packages=("$@")

    # Detect OS distribution and install packages
    if command -v dnf >/dev/null 2>&1; then
        # Red Hat/Fedora/CentOS systems
        sudo dnf install -y "${packages[@]}"
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu/Debian systems
        sudo apt-get update
        sudo apt-get install -y "${packages[@]}"
    else
        echo "Error: No supported package manager found (dnf, and apt-get)"
        exit 1
    fi
}

# Exit immediately on command failure and enable verbose execution tracing
set -e
set -x

################################################################################
# Global Configuration Variables
################################################################################

# Path to directory containing this script (benchpress packages)
BPKGS_ADSIM_ROOT="$(dirname "$(readlink -f "$0")")"

# Root directory of the Benchpress benchmarking framework
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_ADSIM_ROOT/../..")"

# Output directory for final benchmark executables and libraries
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks/adsim"

# Temporary directory for intermediate build artifacts
BUILD_DIR="${BENCHPRESS_ROOT}/build"

################################################################################
# Setup Functions
################################################################################

# Create and prepare build directories for compilation process
setup_directories() {
  echo "[SETUP] Setting up directories for the build process..."

  # Create final benchmark output directory
  mkdir -p ${BENCHMARKS_DIR}

  # Clean and recreate temporary build directory
  rm -rf ${BUILD_DIR}
  mkdir -p ${BUILD_DIR}

  # Enter build directory for subsequent operations
  pushd ${BUILD_DIR} || exit
}

# Build AdSim server by copying sources and invoking dependency/main build scripts
build_adsim() {
  # Copy build scripts and configuration to build directory
  cp "${BENCHPRESS_ROOT}/packages/adsim/config.sh" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/build-deps.sh" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/build-adsim.sh" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/install_adsim.sh" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/install_fbgemm.sh" "${BUILD_DIR}"

  # Copy source code, patches, and build configurations
  cp -R "${BENCHPRESS_ROOT}/packages/adsim/src" "${BUILD_DIR}/adsim"
  cp -R "${BENCHPRESS_ROOT}/packages/adsim/patches" "${BUILD_DIR}"
  cp -R "${BENCHPRESS_ROOT}/packages/adsim/buildfiles" "${BUILD_DIR}"

  # Execute dependency build followed by main AdSim compilation
  pushd "${BUILD_DIR}" || exit
  ./build-deps.sh    # Build C++ libraries and FBGEMM
  ./build-adsim.sh   # Build AdSim server with compression benchmarks
}

# Build Treadmill load testing framework for AdSim performance evaluation
build_treadmill() {
  # Clone Meta's archived Treadmill load testing tool
  git clone https://github.com/facebookarchive/treadmill.git
  pushd treadmill || exit 1

  # Apply AdSim-specific patches for integration
  patch -p1 --follow-symlinks --forward < "${BENCHPRESS_ROOT}/packages/adsim/patches/treadmill.patch"

  # Make build script executable and compile Treadmill
  sudo chmod u+x build.sh
  ./build.sh
  popd || exit 1
}

# Copy built executables, libraries, and configurations to final benchmark directory
post_build() {
  # Copy main executables: AdSim server and Treadmill load generator
  cp "${BUILD_DIR}/adsim/build/cpp2/server/adsim_server" "${BENCHMARKS_DIR}"
  cp "${BUILD_DIR}/treadmill/build/services/adsim/treadmill_adsim" "${BENCHMARKS_DIR}"

  # Create library directory and copy all shared libraries
  mkdir -p "${BENCHMARKS_DIR}/lib/"
  cp ${BUILD_DIR}/staging/lib/*.so* "${BENCHMARKS_DIR}/lib/"
  cp ${BUILD_DIR}/staging/lib64/*.so* "${BENCHMARKS_DIR}/lib/"

  # Copy runtime configurations, Python scripts, and QPS search tool
  cp -R "${BENCHPRESS_ROOT}/packages/adsim/configs" "${BENCHMARKS_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/run_adsim.py" "${BENCHMARKS_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/adsim_config.py" "${BENCHMARKS_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/adsim/qps_search.sh" "${BENCHMARKS_DIR}"


  cat "${BENCHPRESS_ROOT}/packages/deser/model_a_part_"*.dist > "${BENCHMARKS_DIR}/deser_model_a.dist"
  cat "${BENCHPRESS_ROOT}/packages/deser/model_b_part_"*.dist > "${BENCHMARKS_DIR}/deser_model_b.dist"

  cp "${BENCHPRESS_ROOT}/packages/rebatch/model_a.dist" "${BENCHMARKS_DIR}/rebatch_model_a.dist"
  cp "${BENCHPRESS_ROOT}/packages/rebatch/model_b.dist" "${BENCHMARKS_DIR}/rebatch_model_b.dist"

  # Install patchelf and set runtime library paths for executables
  install_packages patchelf
  patchelf --set-rpath "${BENCHMARKS_DIR}/lib" ${BENCHMARKS_DIR}/adsim_server
  patchelf --set-rpath "${BENCHMARKS_DIR}/lib" ${BENCHMARKS_DIR}/treadmill_adsim
}

# Execute build pipeline: setup -> dependencies -> AdSim -> Treadmill -> packaging
setup_directories
build_adsim
build_treadmill
post_build
