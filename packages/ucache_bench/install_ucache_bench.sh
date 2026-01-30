#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Open Source UCacheBench Installation Script
# This script downloads and builds all dependencies from GitHub, then builds UCacheBench using CMake

set -Eeuo pipefail

echo "=============================================="
echo "UCacheBench Open Source Installation Script"
echo "=============================================="

# Script configuration
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$SCRIPT_DIR/../..")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"
UCACHE_BENCH_DIR="$BENCHPRESS_ROOT/benchmarks/ucache_bench"
DEPS_DIR="$UCACHE_BENCH_DIR/deps"
STAGING_DIR="$UCACHE_BENCH_DIR/staging"
BUILD_DIR="$SCRIPT_DIR/build"

# Source OS distro detection helpers
# shellcheck source=../common/os-distro.sh
source "${COMMON_DIR}/os-distro.sh"

# Number of parallel jobs for compilation
NPROC=${NPROC:-$(nproc)}

echo "Configuration:"
echo "  Benchpress root: $BENCHPRESS_ROOT"
echo "  UCacheBench directory: $UCACHE_BENCH_DIR"
echo "  Dependencies directory: $DEPS_DIR"
echo "  Staging directory: $STAGING_DIR"
echo "  Build directory: $BUILD_DIR"
echo "  Parallel jobs: $NPROC"
echo "  OS Distro: $(get_os_distro_id) (family: $(get_os_distro_family))"
echo ""

# Create directories
mkdir -p "$DEPS_DIR"
mkdir -p "$STAGING_DIR"
mkdir -p "$BUILD_DIR"
mkdir -p "$UCACHE_BENCH_DIR/server"
mkdir -p "$UCACHE_BENCH_DIR/client"

# Helper function to clone or update a git repository
# Supports both branches (e.g., "main") and tags (e.g., "11.0.2")
clone_or_update() {
    local repo_url="$1"
    local repo_dir="$2"
    local ref="${3:-main}"
    local commit="${4:-}"

    if [ -d "$repo_dir" ]; then
        echo "  Updating $repo_dir..."
        cd "$repo_dir"
        git fetch origin --tags
        # Try checkout as-is first (works for tags and local branches),
        # then try as remote branch, then as tag with prefix
        if git checkout "$ref" 2>/dev/null; then
            git pull origin "$ref" 2>/dev/null || true
        elif git checkout -B "$ref" "origin/$ref" 2>/dev/null; then
            git pull origin "$ref" || true
        elif git checkout "tags/$ref" 2>/dev/null; then
            : # Tags don't need pull
        else
            echo "  WARNING: Could not checkout $ref"
        fi
    elif [ -z "$commit" ]; then
        echo "  Cloning $repo_url to $repo_dir (shallow)..."
        git clone --depth 1 --branch "$ref" "$repo_url" "$repo_dir"
    else
        echo "  Cloning $repo_url to $repo_dir..."
        git clone --branch "$ref" "$repo_url" "$repo_dir"
    fi

    if [ -n "$commit" ]; then
        echo "  Checking out commit $commit..."
        pushd "$repo_dir" || exit 1
        git checkout "$commit"
        popd || exit 1
    fi
}

# Helper function to build with cmake
cmake_build() {
    local src_dir="$1"
    local build_dir="$2"
    shift 2

    mkdir -p "$build_dir"
    cd "$build_dir"
    cmake "$src_dir" \
        -DCMAKE_INSTALL_PREFIX="$STAGING_DIR" \
        -DCMAKE_PREFIX_PATH="$STAGING_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        "$@"
    make -j"$NPROC"
    make install
}

echo "=============================================="
echo "Step 1: Installing System Dependencies"
echo "=============================================="

# Install system packages based on OS distro
echo "Installing required system packages..."

if distro_is_like "ubuntu" || distro_is_like "debian"; then
    echo "Detected Ubuntu/Debian-based system, using apt..."
    apt-get update
    apt-get install -y \
        cmake ninja-build g++ libssl-dev libboost-all-dev libevent-dev \
        libdouble-conversion-dev libgoogle-glog-dev libgflags-dev \
        libiberty-dev liblz4-dev liblzma-dev libsnappy-dev zlib1g-dev \
        libjemalloc-dev libsodium-dev autoconf automake libtool pkg-config \
        git flex bison libfmt-dev libunwind-dev python3-dev libzstd-dev \
        ragel libatomic1 libbz2-dev libnuma-dev libdwarf-dev libelf-dev \
        libaio-dev liburing-dev
elif distro_is_like "centos" || distro_is_like "rhel" || distro_is_like "fedora"; then
    echo "Detected CentOS/RHEL/Fedora-based system, using dnf..."
    dnf install -y \
        cmake ninja-build gcc-c++ openssl-devel boost-devel libevent-devel \
        double-conversion-devel glog-devel gflags-devel \
        lz4-devel xz-devel snappy-devel zlib-devel \
        jemalloc-devel libsodium-devel autoconf automake libtool pkgconfig \
        git flex bison fmt-devel libunwind-devel python3-devel libzstd-devel \
        ragel libatomic bzip2-devel numactl-libs numactl-devel libdwarf-devel \
        elfutils-libelf-devel libaio-devel liburing-devel binutils-devel
else
    echo "WARNING: Unsupported OS distribution: $(get_os_distro_id)"
    echo "Please install the following packages manually:"
    echo "  cmake, ninja, g++/gcc-c++, openssl-dev, boost-dev, libevent-dev,"
    echo "  double-conversion-dev, glog-dev, gflags-dev, lz4-dev, lzma-dev,"
    echo "  snappy-dev, zlib-dev, jemalloc-dev, libsodium-dev, autoconf,"
    echo "  automake, libtool, pkg-config, git, flex, bison, fmt-dev,"
    echo "  libunwind-dev, python3-dev"
    echo ""
    echo "Attempting to continue anyway..."
fi

echo ""
echo "=============================================="
echo "Step 2: Downloading Third-Party Dependencies"
echo "=============================================="

cd "$DEPS_DIR"

# 1. Boost (required by folly, fizz, wangle, fbthrift, mcrouter)
echo ""
echo "[1/13] Downloading Boost..."
BOOST_VERSION="1.75.0"
BOOST_VERSION_UNDERSCORE="1_75_0"
BOOST_ARCHIVE="boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz"
if [ ! -f "$DEPS_DIR/$BOOST_ARCHIVE" ]; then
    echo "  Downloading Boost $BOOST_VERSION..."
    wget -O "$DEPS_DIR/$BOOST_ARCHIVE" "$BOOST_URL"
fi
if [ ! -d "$DEPS_DIR/boost_${BOOST_VERSION_UNDERSCORE}" ]; then
    echo "  Extracting Boost..."
    cd "$DEPS_DIR"
    tar -xzf "$BOOST_ARCHIVE"
fi

# 2. libsodium (crypto library required by fizz)
echo ""
echo "[2/13] Downloading libsodium..."
clone_or_update "https://github.com/jedisct1/libsodium.git" "$DEPS_DIR/libsodium" "1.0.20-RELEASE"

# 3. xxhash (fast hash library required by folly/fbthrift)
echo ""
echo "[3/13] Downloading xxhash..."
clone_or_update "https://github.com/Cyan4973/xxHash.git" "$DEPS_DIR/xxhash" "v0.8.2"

# 4. fmt (formatting library)
echo ""
echo "[4/13] Downloading fmt..."
clone_or_update "https://github.com/fmtlib/fmt.git" "$DEPS_DIR/fmt" "11.0.2"

WDL_VERSION_TAG="v2025.12.01.00"
# 5. folly (Facebook Open-source Library)
echo ""
echo "[5/13] Downloading folly..."
clone_or_update "https://github.com/facebook/folly.git" "$DEPS_DIR/folly" "main" "2de2c909323ea65ad0b0acbc398519608c647d20"

# 6. fizz (TLS 1.3 library)
echo ""
echo "[6/13] Downloading fizz..."
clone_or_update "https://github.com/facebookincubator/fizz.git" "$DEPS_DIR/fizz" "$WDL_VERSION_TAG"

# 7. wangle (networking library)
echo ""
echo "[7/13] Downloading wangle..."
clone_or_update "https://github.com/facebook/wangle.git" "$DEPS_DIR/wangle" "$WDL_VERSION_TAG"

# 8. fbthrift (Thrift for Facebook)
echo ""
echo "[8/13] Downloading fbthrift..."
clone_or_update "https://github.com/facebook/fbthrift.git" "$DEPS_DIR/fbthrift" "$WDL_VERSION_TAG"

# 9. mvfst (QUIC protocol library - required by fbthrift)
echo ""
echo "[9/13] Downloading mvfst..."
clone_or_update "https://github.com/facebook/mvfst.git" "$DEPS_DIR/mvfst" "$WDL_VERSION_TAG"

# 10. mcrouter (Memcache router)
echo ""
echo "[10/13] Downloading mcrouter..."
clone_or_update "https://github.com/facebook/mcrouter.git" "$DEPS_DIR/mcrouter" "main" "cbe0bae209cea65a518606ece7d4fd88d82fd5c9"

# 11. CacheLib (Facebook's caching engine)
echo ""
echo "[11/13] Downloading CacheLib..."
# CacheLib's v2025.12.01.00 tag could not build in OSS, so use a specific commit (latest as of 2025-12-11)
CACHELIB_VERSION="2812ee398471ff627b937702dd48d7b1b5553564"
clone_or_update "https://github.com/facebook/CacheLib.git" "$DEPS_DIR/CacheLib" "main" "$CACHELIB_VERSION"

# 12. sparsemap (header-only library required by CacheLib)
echo ""
echo "[12/13] Downloading sparsemap..."
clone_or_update "https://github.com/Tessil/sparse-map.git" "$DEPS_DIR/sparsemap" "v0.7.0"

# 13. googletest (required by mcrouter and CacheLib)
echo ""
echo "[13/14] Downloading googletest..."
clone_or_update "https://github.com/google/googletest.git" "$DEPS_DIR/googletest" "v1.14.0"

# 14. fast_float (required by folly for fast float parsing)
echo ""
echo "[14/15] Downloading fast_float..."
clone_or_update "https://github.com/fastfloat/fast_float.git" "$DEPS_DIR/fast_float" "v8.1.0"

# 15. libaegis (required by fizz on aarch64 for AEGIS cipher support)
ARCH="$(uname -m)"
if [ "$ARCH" = "aarch64" ]; then
    echo ""
    echo "[15/15] Downloading libaegis (required on aarch64)..."
    clone_or_update "https://github.com/aegis-aead/libaegis.git" "$DEPS_DIR/libaegis" "0.4.2"
else
    echo ""
    echo "[15/15] Skipping libaegis (only required on aarch64)..."
fi

echo ""
echo "=============================================="
echo "Step 3: Building Third-Party Dependencies"
echo "=============================================="

# Set common CMake flags
COMMON_CMAKE_FLAGS="-DCMAKE_POSITION_INDEPENDENT_CODE=ON"

# 3.0 Build libaegis (required by fizz on aarch64)
# This must be built before fizz as fizz needs aegis.h
ARCH="$(uname -m)"
if [ "$ARCH" = "aarch64" ]; then
    echo ""
    echo "[0/13] Building libaegis (required on aarch64)..."
    cd "$DEPS_DIR"

    # Download Zig compiler (required to build libaegis)
    if [ ! -d "$DEPS_DIR/zig" ]; then
        echo "  Downloading Zig compiler..."
        wget -q https://ziglang.org/download/0.15.2/zig-aarch64-linux-0.15.2.tar.xz
        tar xf zig-aarch64-linux-0.15.2.tar.xz
        mv zig-aarch64-linux-0.15.2 zig
        rm -f zig-aarch64-linux-0.15.2.tar.xz
    fi

    cd "$DEPS_DIR/libaegis"
    ../zig/zig build -Drelease -Dfavor-performance

    # Install libaegis headers and library to staging directory
    echo "  Installing libaegis to staging directory..."
    mkdir -p "$STAGING_DIR/include"
    mkdir -p "$STAGING_DIR/lib"
    cp -r ./include/* "$STAGING_DIR/include/" 2>/dev/null || cp -r ./zig-out/include/* "$STAGING_DIR/include/" 2>/dev/null || true
    cp ./zig-out/lib/libaegis.a "$STAGING_DIR/lib/" 2>/dev/null || true

    # Create pkg-config file for libaegis
    mkdir -p "$STAGING_DIR/lib/pkgconfig"
    cat > "$STAGING_DIR/lib/pkgconfig/libaegis.pc" << AEGIS_PC_EOF
prefix=$STAGING_DIR
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libaegis
Description: AEGIS authenticated encryption library
Version: 0.4.2
Libs: -L\${libdir} -laegis
Cflags: -I\${includedir}
AEGIS_PC_EOF

    echo "  libaegis installed to $STAGING_DIR"
else
    echo ""
    echo "[0/13] Skipping libaegis build (only required on aarch64)..."
fi

# 3.1 Build Boost (required by folly, fizz, wangle, fbthrift, mcrouter)
echo ""
echo "[1/12] Building Boost..."
cd "$DEPS_DIR/boost_${BOOST_VERSION_UNDERSCORE}"

# Skip if already built
if [ ! -f "$STAGING_DIR/lib/libboost_context.a" ]; then
    # Bootstrap Boost.Build
    if [ ! -f "./b2" ]; then
        ./bootstrap.sh --prefix="$STAGING_DIR" \
            --with-libraries=context,filesystem,program_options,regex,system,thread
    fi

    # Build and install Boost libraries
    ./b2 \
        --prefix="$STAGING_DIR" \
        --build-dir="$(pwd)/build" \
        --without-python \
        variant=release \
        link=static,shared \
        threading=multi \
        cxxflags="-fPIC" \
        -j "$NPROC" \
        install
    echo "  Boost ${BOOST_VERSION} installed to $STAGING_DIR"
else
    echo "  Boost already installed, skipping"
fi

# 3.2 Build libsodium (uses autotools, not CMake)
echo ""
echo "[2/12] Building libsodium..."
cd "$DEPS_DIR/libsodium"
./autogen.sh
./configure --prefix="$STAGING_DIR" --enable-shared=no --with-pic
make -j"$NPROC"
make install

# Create FindSodium.cmake module for CMake to find libsodium
# (libsodium uses autotools and doesn't provide CMake config files)
mkdir -p "$STAGING_DIR/lib/cmake/Sodium"
cat > "$STAGING_DIR/lib/cmake/Sodium/SodiumConfig.cmake" << 'SODIUM_CMAKE_EOF'
# CMake config file for libsodium
# Auto-generated by install_ucache_bench.sh

if(NOT TARGET sodium)
    get_filename_component(_SODIUM_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

    add_library(sodium STATIC IMPORTED)
    set_target_properties(sodium PROPERTIES
        IMPORTED_LOCATION "${_SODIUM_PREFIX}/lib/libsodium.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_SODIUM_PREFIX}/include"
    )

    # Create alias for compatibility
    add_library(sodium::sodium ALIAS sodium)
endif()

set(Sodium_FOUND TRUE)
set(SODIUM_FOUND TRUE)
set(Sodium_LIBRARIES sodium)
set(SODIUM_LIBRARIES sodium)
get_filename_component(Sodium_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../include" ABSOLUTE)
set(SODIUM_INCLUDE_DIR "${Sodium_INCLUDE_DIR}")
SODIUM_CMAKE_EOF

echo "  Created SodiumConfig.cmake for CMake compatibility"

# 3.3 Build xxhash (uses CMake)
echo ""
echo "[3/12] Building xxhash..."
cmake_build "$DEPS_DIR/xxhash/cmake_unofficial" "$DEPS_DIR/xxhash/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_SHARED_LIBS=OFF \
    -DXXHASH_BUILD_XXHSUM=OFF

# Create XxhashConfig.cmake for compatibility (fbthrift expects "Xxhash" not "xxHash")
mkdir -p "$STAGING_DIR/lib/cmake/Xxhash"
cat > "$STAGING_DIR/lib/cmake/Xxhash/XxhashConfig.cmake" << 'XXHASH_CMAKE_EOF'
# CMake config file for xxhash (compatibility wrapper)
# Auto-generated by install_ucache_bench.sh
# This provides "Xxhash" package name that fbthrift expects

get_filename_component(_XXHASH_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Try to find the installed xxHash config first
include("${_XXHASH_PREFIX}/lib/cmake/xxHash/xxHashConfig.cmake" OPTIONAL RESULT_VARIABLE _xxhash_found)

if(NOT _xxhash_found)
    # Fallback: create targets manually
    if(NOT TARGET xxHash::xxhash)
        add_library(xxHash::xxhash STATIC IMPORTED)
        set_target_properties(xxHash::xxhash PROPERTIES
            IMPORTED_LOCATION "${_XXHASH_PREFIX}/lib/libxxhash.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_XXHASH_PREFIX}/include"
        )
    endif()
endif()

set(Xxhash_FOUND TRUE)
set(XXHASH_FOUND TRUE)
XXHASH_CMAKE_EOF

echo "  Created XxhashConfig.cmake for CMake compatibility"

# 3.4 Build googletest (required by mcrouter)
echo ""
echo "[4/12] Building googletest..."
cmake_build "$DEPS_DIR/googletest" "$DEPS_DIR/googletest/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_GMOCK=ON

# 3.5 Build fmt
echo ""
echo "[5/13] Building fmt..."
cmake_build "$DEPS_DIR/fmt" "$DEPS_DIR/fmt/build" \
    $COMMON_CMAKE_FLAGS \
    -DFMT_TEST=OFF \
    -DFMT_DOC=OFF

# 3.6 Build fast_float (header-only library, but uses CMake for installation)
echo ""
echo "[6/13] Building fast_float..."
cmake_build "$DEPS_DIR/fast_float" "$DEPS_DIR/fast_float/build" \
    $COMMON_CMAKE_FLAGS \
    -DFASTFLOAT_TEST=OFF

# 3.7 Build folly
echo ""
echo "[7/13] Building folly..."
cmake_build "$DEPS_DIR/folly" "$DEPS_DIR/folly/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DFOLLY_USE_JEMALLOC=ON

# 3.8 Build fizz
echo ""
echo "[8/13] Building fizz..."
# On aarch64, we have libaegis available; on x86_64, we need to disable the AEGIS backend
ARCH="$(uname -m)"
if [ "$ARCH" = "aarch64" ]; then
    cmake_build "$DEPS_DIR/fizz/fizz" "$DEPS_DIR/fizz/build" \
        $COMMON_CMAKE_FLAGS \
        -DBUILD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF
else
    # On x86_64, libaegis is not available. We need to explicitly disable finding aegis to prevent CMake from finding stale
    #    aegis installations elsewhere on the system
    cmake_build "$DEPS_DIR/fizz/fizz" "$DEPS_DIR/fizz/build" \
        $COMMON_CMAKE_FLAGS \
        -DBUILD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_aegis=ON
fi

# 3.9 Build wangle
echo ""
echo "[9/13] Building wangle..."
cmake_build "$DEPS_DIR/wangle/wangle" "$DEPS_DIR/wangle/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF

# 3.10 Build mvfst (QUIC protocol library - required by fbthrift)
echo ""
echo "[10/13] Building mvfst..."
cmake_build "$DEPS_DIR/mvfst" "$DEPS_DIR/mvfst/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF

# 3.11 Build fbthrift
echo ""
if ! [ -f "$STAGING_DIR/bin/thrift1" ]; then
    echo "[11/13] Building fbthrift..."
    cmake_build "$DEPS_DIR/fbthrift" "$DEPS_DIR/fbthrift/build" \
        $COMMON_CMAKE_FLAGS \
        -DBUILD_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -Dthriftpy=OFF \
        -Dthriftpy3=OFF
fi

# 3.12 Build mcrouter
echo ""
echo "[12/13] Building mcrouter..."
cd "$DEPS_DIR/mcrouter"

# Add staging bin to PATH so configure can find thrift1 compiler
export PATH="$STAGING_DIR/bin:$PATH"
export PKG_CONFIG_PATH="$STAGING_DIR/lib/pkgconfig:$STAGING_DIR/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"

# mcrouter uses autotools
cd "$DEPS_DIR/mcrouter/mcrouter"

autoreconf -ivf
./configure \
    --prefix="$STAGING_DIR" \
    --enable-shared=no \
    --with-pic \
    CXXFLAGS="-I$STAGING_DIR/include" \
    LIBS="-luring" \
    LDFLAGS="-L$STAGING_DIR/lib -L$STAGING_DIR/lib64 -Wl,-rpath,$STAGING_DIR/lib -Wl,-rpath,$STAGING_DIR/lib64" \
    FBTHRIFT_BIN="$STAGING_DIR/bin" \
    PKG_CONFIG_PATH="$STAGING_DIR/lib/pkgconfig:$STAGING_DIR/lib64/pkgconfig:${PKG_CONFIG_PATH:-}" \
    PYTHON="/usr/bin/python3" \
    INSTALL_DIR="$STAGING_DIR"
make -j"$NPROC"
make install

# Note: mcrouter headers will be included from source directory during UCacheBench build
# We'll pass MCROUTER_DIR to CMake when building UCacheBench

# 3.12 Build CacheLib
echo ""
echo "[12/12] Building CacheLib..."

# First, install sparsemap headers (header-only library)
echo "  Installing sparsemap headers..."
mkdir -p "$STAGING_DIR/include"
cp -r "$DEPS_DIR/sparsemap/include/"* "$STAGING_DIR/include/"

# Apply CacheLib patch to fix missing GenericPiecesBase.cpp in CMakeLists.txt
# This is a known issue where GenericPiecesBase.cpp is not included in cachelib_common library
CACHELIB_COMMON_CMAKE="$DEPS_DIR/CacheLib/cachelib/common/CMakeLists.txt"
if ! grep -q "GenericPiecesBase.cpp" "$CACHELIB_COMMON_CMAKE"; then
    echo "  Patching CacheLib to include GenericPiecesBase.cpp..."
    sed -i 's|piecewise/GenericPieces.cpp|piecewise/GenericPieces.cpp\n  piecewise/GenericPiecesBase.cpp|' "$CACHELIB_COMMON_CMAKE"
fi

# Build CacheLib using cmake
cmake_build "$DEPS_DIR/CacheLib/cachelib" "$DEPS_DIR/CacheLib/build" \
    $COMMON_CMAKE_FLAGS \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF

echo ""
echo "=============================================="
echo "Step 4: Building UCacheBench"
echo "=============================================="

cd "$BUILD_DIR"

echo "Configuring UCacheBench with CMake..."
export STAGING_DIR="$STAGING_DIR"
export DEPS_DIR="$DEPS_DIR"

# Fix include paths in generated protocol files (pre-generated by carbon_compiler)
# These files have internal fbcode paths that need to be fixed for OSS builds
echo "  Fixing include paths in generated protocol files..."
PROTOCOL_GEN_DIR="$SCRIPT_DIR/protocol/gen"
for f in "$PROTOCOL_GEN_DIR"/*.cpp "$PROTOCOL_GEN_DIR"/*.h "$PROTOCOL_GEN_DIR"/*.thrift; do
    if [ -f "$f" ]; then
        # Fix internal fbcode paths to relative paths
        sed -i 's|#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/|#include "|g' "$f"
        sed -i 's|#include "cea/chips/benchpress/packages/ucache_bench/server/|#include "../server/|g' "$f"
        # Fix thrift include paths (for .thrift files)
        sed -i 's|include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/|include "|g' "$f"
    fi
done

# Add missing include for Fields.h in UcacheBenchMessages.h
# The generated code uses ArithmeticLike, IsMixin, etc. which are defined in Fields.h
if ! grep -q "mcrouter/lib/carbon/Fields.h" "$PROTOCOL_GEN_DIR/UcacheBenchMessages.h"; then
    echo "  Adding missing Fields.h include to UcacheBenchMessages.h..."
    sed -i '/#include <mcrouter\/lib\/carbon\/Result.h>/a #include <mcrouter/lib/carbon/Fields.h>' "$PROTOCOL_GEN_DIR/UcacheBenchMessages.h"
fi

# The generated RoutingGroups.h uses template specializations for ArithmeticLike, GetLike, etc.
# These template classes are defined in mcrouter/lib/carbon/RoutingGroups.h
if ! grep -q "mcrouter/lib/carbon/RoutingGroups.h" "$PROTOCOL_GEN_DIR/UcacheBenchRoutingGroups.h"; then
    echo "  Adding missing RoutingGroups.h include to UcacheBenchRoutingGroups.h..."
    sed -i '/#pragma once/a\
\
#include <mcrouter/lib/carbon/RoutingGroups.h>' "$PROTOCOL_GEN_DIR/UcacheBenchRoutingGroups.h"
fi

cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$STAGING_DIR" \
    -DBUILD_SERVER=ON \
    -DBUILD_CLIENT=ON

echo "Building UCacheBench..."
make -j"$NPROC"

echo ""
echo "=============================================="
echo "Step 5: Installing UCacheBench Binaries"
echo "=============================================="

# Copy binaries to benchmark directory
echo "Copying binaries..."
if [ -f "$BUILD_DIR/server/ucachebench_server" ]; then
    cp "$BUILD_DIR/server/ucachebench_server" "$UCACHE_BENCH_DIR/server/"
    chmod +x "$UCACHE_BENCH_DIR/server/ucachebench_server"
    echo "  Installed: $UCACHE_BENCH_DIR/server/ucachebench_server"
else
    echo "  WARNING: ucachebench_server binary not found"
fi

if [ -f "$BUILD_DIR/client/ucachebench_client" ]; then
    cp "$BUILD_DIR/client/ucachebench_client" "$UCACHE_BENCH_DIR/client/"
    chmod +x "$UCACHE_BENCH_DIR/client/ucachebench_client"
    echo "  Installed: $UCACHE_BENCH_DIR/client/ucachebench_client"
else
    echo "  WARNING: ucachebench_client binary not found"
fi

echo ""
echo "=============================================="
echo "UCacheBench Installation Complete!"
echo "=============================================="
echo ""
echo "Binaries are installed at:"
echo "  Server: $UCACHE_BENCH_DIR/server/ucachebench_server"
echo "  Client: $UCACHE_BENCH_DIR/client/ucachebench_client"
echo ""
echo "Usage:"
echo "  Server: $UCACHE_BENCH_DIR/server/ucachebench_server --help"
echo "  Client: $UCACHE_BENCH_DIR/client/ucachebench_client --help"
echo ""
echo "Example (single-node test):"
echo "  # Start server on port 11212"
echo "  $UCACHE_BENCH_DIR/server/ucachebench_server --port=11212 --memory_mb=1024"
echo ""
echo "  # Run client benchmark"
echo "  $UCACHE_BENCH_DIR/client/ucachebench_client --server_host=localhost --server_port=11212"
echo ""
echo "Benchpress integration:"
echo "  benchpress ucache_bench_default --server-hostname=<hostname>"
echo "  benchpress ucache_bench_custom --server-hostname=<hostname>"
echo ""
