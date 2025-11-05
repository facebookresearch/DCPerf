#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "Building Proxygen Python Binding"
echo "=========================================="

# Check for PROXYGEN_INSTALL_DIR environment variable
if [ -z "$PROXYGEN_INSTALL_DIR" ]; then
    echo "WARNING: PROXYGEN_INSTALL_DIR not set, using default: \$HOME/proxygen/staging"
    echo ""
    echo "To specify a custom location, set PROXYGEN_INSTALL_DIR:"
    echo "  export PROXYGEN_INSTALL_DIR=/path/to/proxygen/staging"
    echo "  ./build.sh"
    echo ""
else
    echo "Using PROXYGEN_INSTALL_DIR: $PROXYGEN_INSTALL_DIR"
fi

# Check for required tools
echo ""
echo "Checking for required dependencies..."

if ! python3 -c "import pybind11" 2>/dev/null; then
    echo "Installing pybind11..."
    pip install pybind11
fi

# Build the extension
echo ""
echo "Building C++ extension module..."
python3 setup.py build_ext --inplace -g

if [ $? -eq 0 ]; then
    echo ""
    echo "=========================================="
    echo "Build successful!"
    echo "=========================================="
    echo ""
    echo "To test the basic server:"
    echo "  python3 example_server.py"
    echo ""
    echo "To test with Django:"
    echo "  python3 django_server.py"
    echo ""
    echo "To install system-wide:"
    echo "  python3 setup.py install"
    echo ""
    echo "To build with a custom Proxygen location:"
    echo "  export PROXYGEN_INSTALL_DIR=/path/to/proxygen/staging"
    echo "  python3 setup.py build_ext --inplace"
    echo ""
    echo "Or use CLI arguments:"
    echo "  python3 setup.py build_ext --inplace --proxygen-dir=/path/to/proxygen/staging"
    echo ""
    echo "Additional CLI options:"
    echo "  --extra-include-dirs=/path1:/path2  # Additional include directories"
    echo "  --extra-lib-dirs=/path1:/path2      # Additional library directories"
    echo ""
else
    echo ""
    echo "=========================================="
    echo "Build failed!"
    echo "=========================================="
    exit 1
fi
