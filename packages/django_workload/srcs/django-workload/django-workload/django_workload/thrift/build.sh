#!/bin/bash
# Build script for compiling Thrift definitions using OSS fbthrift

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
FBTHRIFT_PREFIX="${FBTHRIFT_PREFIX:-/home/wsu/proxygen/proxygen/_build/deps}"
THRIFT_COMPILER="${FBTHRIFT_PREFIX}/bin/thrift1"

echo "==> DjangoBench V2 Thrift Build Script"
echo "==> FBTHRIFT_PREFIX: ${FBTHRIFT_PREFIX}"
echo "==> Thrift compiler: ${THRIFT_COMPILER}"
echo "==> Build directory: ${BUILD_DIR}"

# Check if thrift compiler exists
if [ ! -f "${THRIFT_COMPILER}" ]; then
    echo "ERROR: Thrift compiler not found at ${THRIFT_COMPILER}"
    exit 1
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Run CMake
echo "==> Running CMake..."
cmake "${SCRIPT_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFBTHRIFT_PREFIX="${FBTHRIFT_PREFIX}" \
    -DCMAKE_INSTALL_PREFIX="${SCRIPT_DIR}/install"

# Build
echo "==> Building Thrift bindings..."
make -j"$(nproc)"

echo "==> Build complete!"
echo "==> Generated files in: $BUILD_DIR/gen-py3/"

echo ""
echo "✅ Pure Python Thrift bindings generated successfully!"
echo "✅ No Cython compilation needed (py:asyncio generates pure Python)"
ls -la "${BUILD_DIR}/gen-py3/mock_services/" 2>/dev/null || echo "⚠️  Note: Check that files were generated in ${BUILD_DIR}/gen-py3/"

# Optional: Install
if [ "$1" == "install" ]; then
    echo "==> Installing..."
    make install
fi
