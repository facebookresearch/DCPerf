#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# install_deser.sh: Installation script for Deserialization benchmark
# This script builds and installs the Folly library dependency and the Deserialization benchmark


# Exit immediately if a command exits with a non-zero status
set -e
# Print each command before executing (verbose mode for debugging)
set -x


################################################################################
# Global Configuration Variables
################################################################################

# Directory where benchmark executables will be stored
BENCHMARKS_DIR="$(pwd)/benchmarks/deser"

# Temporary directory for build artifacts
BUILD_DIR="${BENCHMARKS_DIR}/build"

# Specific Folly library version to ensure compatibility
FOLLY_VERSION="v2025.06.23.00"

# Path to directory containing this script
BPKGS_DESER_ROOT="$(dirname "$(readlink -f "$0")")"

# Root directory of the Benchpress project
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_DESER_ROOT/../..")"


################################################################################
# Setup Functions
################################################################################

# Function to set up directories for the build process
# Creates necessary directories and prepares the build environment
setup_directories() {
  echo "[SETUP] Setting up directories for the build process..."

  # Create the benchmarks directory if it doesn't exist
  # shellcheck disable=SC2086
  mkdir -p ${BENCHMARKS_DIR}

  # Remove any existing build directory to ensure a clean build environment
  # shellcheck disable=SC2086
  rm -rf ${BUILD_DIR}

  # Create a new build directory for the current build process
  # shellcheck disable=SC2086
  mkdir -p ${BUILD_DIR}

  # Change to build directory and push current dir to stack for later return
  # shellcheck disable=SC2086
  pushd ${BUILD_DIR} || exit
}

# Install system dependencies based on OS type
install_system_dependencies() {
  echo "[SETUP] Installing system dependencies..."

  # Detect OS and install dependencies (supporting only Ubuntu and CentOS)
  if command -v apt-get >/dev/null 2>&1; then
    # Ubuntu
    echo "Detected Ubuntu system, installing Ubuntu dependencies"
    sudo apt-get update
    sudo apt-get install -y libssl-dev
    sudo apt-get install -y libjemalloc-dev
    sudo apt-get install -y clang
  elif command -v dnf >/dev/null 2>&1; then
    # CentOS
    echo "Detected CentOS system, installing CentOS dependencies"
    sudo dnf install -y openssl-devel
    sudo dnf install -y jemalloc
    sudo dnf install -y clang
  else
    echo "ERROR: This script only supports Ubuntu (apt-get) and CentOS (dnf)"
    exit 1
  fi

  echo "System dependencies installed successfully"
}

# Check if Python 3.6+ is installed, install if not available
check_python_version() {
  echo "[SETUP] Verifying Python 3.6+ installation..."

  # Check if python3 is installed and its version
  if command -v python3 >/dev/null 2>&1; then
    python_version=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    echo "Found Python version: $python_version"

    # Compare version with 3.6
    if python3 -c 'import sys; exit(0 if sys.version_info >= (3, 6) else 1)'; then
      echo "Python 3.6+ requirement satisfied"
      return 0
    else
      echo "Python version too old: $python_version, need 3.6+"
    fi
  else
    echo "Python 3 not found"
  fi

  # Install Python 3 if not found or version too old
  echo "Installing Python 3.6+..."

  # Detect OS and install Python 3 (supporting only Ubuntu and CentOS)
  if command -v apt-get >/dev/null 2>&1; then
    # Ubuntu
    echo "Detected Ubuntu system, using apt-get"
    sudo apt-get update
    sudo apt-get install -y python3 python3-dev python3-pip
  elif command -v dnf >/dev/null 2>&1; then
    # CentOS
    echo "Detected CentOS system, using dnf"
    sudo dnf install -y python3 python3-devel python3-pip
  else
    echo "ERROR: This script only supports Ubuntu (apt-get) and CentOS (dnf)"
    echo "Please install Python 3.6+ manually and run this script again"
    exit 1
  fi

  # Verify installation was successful
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: Failed to install Python 3"
    exit 1
  fi

  # Verify version is 3.6+
  if ! python3 -c 'import sys; exit(0 if sys.version_info >= (3, 6) else 1)'; then
    echo "ERROR: Installed Python version is still below 3.6"
    exit 1
  fi

  echo "Python 3.6+ successfully installed"
}

# Build and install Folly library (required dependency for Deserialization benchmark)
build_folly() {
  # Clone specific version of Folly with all submodules
  git clone -b $FOLLY_VERSION --recursive https://github.com/facebook/folly.git folly-${FOLLY_VERSION}

  pushd folly-${FOLLY_VERSION}
  # Install system dependencies required by Folly
  sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive
  # Build Folly with system packages allowed and using our build directory
  python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build --scratch-path "${BUILD_DIR}"

  popd
}

# Build the Deserialization benchmark
build_benchmark() {
  echo -e "Building Deser benchmark..."

  # Copy source files to build directory
  cp "${BENCHPRESS_ROOT}/packages/deser/DeserBenchmark.cpp" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/deser/memwrap.cpp" "${BUILD_DIR}"
  cp "${BENCHPRESS_ROOT}/packages/deser/CMakeLists.txt" "${BUILD_DIR}"

  # Assemble model_a.dist from parts
  cat "${BENCHPRESS_ROOT}/packages/deser/model_a_part_"*.dist > "${BENCHMARKS_DIR}/model_a.dist"

  # Assemble model_b.dist from parts
  cat "${BENCHPRESS_ROOT}/packages/deser/model_b_part_"*.dist > "${BENCHMARKS_DIR}/model_b.dist"

  # Configure CMake for optimized release build
  cmake -DCMAKE_CXX_FLAGS="-O3 -g1" .
  # Build using all available cores
  make -j

  # Copy the compiled benchmark to the benchmarks directory
  cp deser_bench "${BENCHMARKS_DIR}"

  popd || exit
}


################################################################################
# Main Functions
################################################################################

# Main installation process orchestrating the build workflow
main() {
  # Prepare the build environment
  setup_directories

  # Verify Python 3.6+ is installed
  check_python_version

  # Install system dependencies (including OpenSSL)
  install_system_dependencies

  # Build and install Folly library dependency
  build_folly

  # Build the deserilization benchmark executable
  build_benchmark

}

# Run the main function
main
