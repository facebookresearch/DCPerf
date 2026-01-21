#!/bin/bash
# shellcheck disable=SC1091,SC2027,SC2086,SC2155,SC2010
# AdSim dependency builder - compiles Facebook C++ libraries and FBGEMM for ad simulation

# Useful constants
COLOR_RED="\033[0;31m"
COLOR_GREEN="\033[0;32m"
COLOR_OFF="\033[0m"

# Cross-platform package installation function with package name mapping
install_packages() {
    # Detect OS distribution and install packages with appropriate names
    if command -v dnf >/dev/null 2>&1; then
        # Red Hat/Fedora/CentOS systems
        # Note: fast_float and boost are built from source since system packages are too old
        sudo dnf install -y clang jemalloc-devel xxhash-devel bzip2-devel libomp-devel gengetopt gcc-toolset-14-libatomic-devel python3-devel gtest-devel \
            double-conversion double-conversion-devel libsodium-devel \
            gflags-devel glog-devel libunwind-devel libevent-devel lz4-devel libzstd-devel snappy-devel xz-devel binutils-devel
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu/Debian systems - map package names to Ubuntu equivalents
        # Note: fast_float and boost are built from source since system packages are too old
        sudo apt-get update
        sudo apt-get install -y clang libjemalloc-dev libxxhash-dev libbz2-dev libomp-dev gengetopt libatomic1 python3-dev libgtest-dev \
            libdouble-conversion-dev libsodium-dev \
            libgflags-dev libgoogle-glog-dev libunwind-dev libevent-dev liblz4-dev libzstd-dev libsnappy-dev liblzma-dev libiberty-dev
    else
        echo -e "${COLOR_RED}Error: No supported package manager found (dnf and apt-get)${COLOR_OFF}"
        exit 1
    fi
}

# Load build configuration and environment variables
source config.sh

# Install system dependencies
install_packages

# Clean build directories if force rebuild is requested
if [ "$FORCE_REBUILD" = "1" ] && [ -d "${ADSIM_DEPS_DIR}" ]; then
    rm -rf "${ADSIM_DEPS_DIR}" "${ADSIM_STAGING_DIR}" "${FBGEMM_STAGING_DIR}"
fi

# Create build and staging directories
mkdir -p "${ADSIM_DEPS_DIR}" "${ADSIM_STAGING_DIR}"

# Set compiler environment for consistent C++20 builds
export CC="${ADSIM_C_COMPILER}"
export CXX="${ADSIM_CXX_COMPILER}"

# Pin Facebook library versions for reproducible builds
# Note: Facebook OSS libraries (folly, fizz, wangle, mvfst, fbthrift, fb303) are released
# together with matching version tags to ensure compatibility
FOLLY_VERSION=v2025.06.23.00
FIZZ_VERSION=v2025.06.23.00
WANGLE_VERSION=v2025.06.23.00
MVFST_VERSION=v2025.06.23.00
FBTHRIFT_VERSION=v2025.06.23.00
FB303_VERSION=v2025.06.23.00

# fast_float v8.0.0+ is required for folly's allow_leading_plus feature
FAST_FLOAT_VERSION=v8.0.0

# Boost 1.83.0+ is required for folly (system packages often have older versions)
BOOST_VERSION=1.83.0
BOOST_VERSION_UNDERSCORE=1_83_0

# Number of parallel build jobs (can be overridden via NUM_BUILD_JOBS environment variable)
JOBS="${NUM_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# Generic function to build Facebook C++ dependencies using CMAKE
# Arguments:
#   $1: dep_name - Name of the dependency (e.g., "folly", "fbthrift", "fb303")
#   $2: repo_url - Git repository URL
#   $3: version - Git tag/version to checkout (optional, uses default branch if empty)
#   $4: cmake_source_subdir - (Optional) Subdirectory containing CMakeLists.txt, defaults to "."
#   $5: extra_cmake_args - (Optional) Additional cmake arguments
build_dependency() {
    local dep_name="$1"
    local repo_url="$2"
    local version="${3:-}"
    local cmake_source_subdir="${4:-.}"
    local extra_cmake_args="${5:-}"

    echo ""
    echo "====================================================================="
    echo "Building and Installing ${dep_name}"
    echo "====================================================================="

    local DEP_DIR="${ADSIM_DEPS_DIR}/${dep_name}"
    local BUILD_DIR="${DEP_DIR}/build"

    # Clone repository if not already present
    if [ ! -d "$DEP_DIR" ]; then
        echo -e "${COLOR_GREEN}[ INFO ] Cloning ${dep_name} repo${COLOR_OFF}"
        git clone "${repo_url}" "$DEP_DIR"
    fi

    cd "$DEP_DIR" || exit

    # Only checkout specific version if provided
    if [ -n "${version}" ]; then
        git fetch --tags
        git checkout "${version}"
    fi

    echo -e "${COLOR_GREEN}Building ${dep_name}${COLOR_OFF}"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR" || exit

    # Create conformance directory for fbthrift (workaround for test generation)
    if [ "${dep_name}" = "fbthrift" ]; then
        mkdir -p "${BUILD_DIR}/thrift/conformance/if"
    fi

    # Determine the source directory for cmake
    local cmake_source_path
    if [ "${cmake_source_subdir}" = "." ]; then
        cmake_source_path=".."
    else
        cmake_source_path="../${cmake_source_subdir}"
    fi

    # Configure with CMAKE (following build_proxygen.sh pattern)
    cmake \
        -DCMAKE_C_COMPILER="${ADSIM_C_COMPILER}" \
        -DCMAKE_CXX_COMPILER="${ADSIM_CXX_COMPILER}" \
        -DCMAKE_PREFIX_PATH="${ADSIM_STAGING_DIR}" \
        -DCMAKE_INSTALL_PREFIX="${ADSIM_STAGING_DIR}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=20 \
        -DCMAKE_C_FLAGS="-g1" \
        -DCMAKE_CXX_FLAGS="-g1" \
        -DBUILD_TESTS=OFF \
        $extra_cmake_args \
        "${cmake_source_path}"

    local cmake_status="$?"
    if [ "$cmake_status" -ne 0 ]; then
        echo -e "${COLOR_RED}CMAKE configuration for ${dep_name} failed!${COLOR_OFF}"
        exit $cmake_status
    fi

    # Build with make
    make -j "$JOBS"

    local build_status="$?"
    if [ "$build_status" -ne 0 ]; then
        echo -e "${COLOR_RED}${dep_name} build failed!${COLOR_OFF}"
        exit $build_status
    fi

    # Install
    make install

    local install_status="$?"
    if [ "$install_status" -eq 0 ]; then
        echo -e "${COLOR_GREEN}${dep_name} is installed${COLOR_OFF}"
    else
        echo -e "${COLOR_RED}${dep_name} install failed!${COLOR_OFF}"
        exit $install_status
    fi

    cd "${ADSIM_PROJ_ROOT}" || exit
}

# =====================================================================
# Build Dependencies
# =====================================================================

echo ""
echo "====================================================================="
echo "Building AdSim Dependencies"
echo "====================================================================="
echo "Using ${JOBS} parallel jobs"
echo "Staging directory: ${ADSIM_STAGING_DIR}"
echo "Dependencies directory: ${ADSIM_DEPS_DIR}"
echo ""

# Build prerequisite dependencies first (following build_proxygen.sh order)
build_dependency "fmt" "https://github.com/fmtlib/fmt.git" "" "." "-DFMT_DOC=OFF -DFMT_TEST=OFF"

# Build glog from source (system package lacks CMake config files needed by folly)
# Note: v0.6.0 is used for compatibility with folly
build_dependency "glog" "https://github.com/google/glog.git" "v0.6.0" "." "-DWITH_GTEST=OFF -DWITH_GFLAGS=ON -DBUILD_SHARED_LIBS=ON"

# Build fast_float from source (v8.0.0+ required for allow_leading_plus feature in folly)
# System packages on most distros are too old - Ubuntu 24.04 has v6.1.1 which lacks this feature
build_dependency "fast_float" "https://github.com/fastfloat/fast_float.git" "${FAST_FLOAT_VERSION}" "." "-DFASTFLOAT_TEST=OFF"

# Build Boost from source (1.83.0+ required for folly)
# System packages often have older versions (e.g., RHEL 8 has 1.75.0)
echo ""
echo "====================================================================="
echo "Building and Installing Boost ${BOOST_VERSION}"
echo "====================================================================="

BOOST_DIR="${ADSIM_DEPS_DIR}/boost"
if [ ! -d "$BOOST_DIR" ]; then
    echo -e "${COLOR_GREEN}[ INFO ] Downloading Boost ${BOOST_VERSION}${COLOR_OFF}"
    mkdir -p "$BOOST_DIR"
    cd "$BOOST_DIR" || exit
    curl -L "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${BOOST_VERSION_UNDERSCORE}.tar.gz" | tar xz --strip-components=1
fi

cd "$BOOST_DIR" || exit

if [ ! -f "b2" ]; then
    echo -e "${COLOR_GREEN}Bootstrapping Boost${COLOR_OFF}"
    ./bootstrap.sh --prefix="${ADSIM_STAGING_DIR}" --with-toolset=clang
fi

echo -e "${COLOR_GREEN}Building Boost libraries${COLOR_OFF}"
./b2 -j "$JOBS" \
    --prefix="${ADSIM_STAGING_DIR}" \
    --with-context \
    --with-coroutine \
    --with-filesystem \
    --with-program_options \
    --with-regex \
    --with-system \
    --with-thread \
    --with-atomic \
    --with-chrono \
    --with-date_time \
    --with-iostreams \
    toolset=clang \
    variant=release \
    link=shared,static \
    threading=multi \
    runtime-link=shared \
    cxxflags="-std=c++20" \
    install

echo -e "${COLOR_GREEN}Boost ${BOOST_VERSION} is installed${COLOR_OFF}"
cd "${ADSIM_PROJ_ROOT}" || exit

# Build core Facebook C++ libraries: async runtime, RPC framework, monitoring
build_dependency "folly" "https://github.com/facebook/folly.git" "${FOLLY_VERSION}"
build_dependency "fizz" "https://github.com/facebookincubator/fizz.git" "${FIZZ_VERSION}" "fizz"
build_dependency "wangle" "https://github.com/facebook/wangle.git" "${WANGLE_VERSION}" "wangle"
build_dependency "mvfst" "https://github.com/facebook/mvfst.git" "${MVFST_VERSION}"
build_dependency "fbthrift" "https://github.com/facebook/fbthrift.git" "${FBTHRIFT_VERSION}"
build_dependency "fb303" "https://github.com/facebook/fb303.git" "${FB303_VERSION}" "." "-DCMAKE_INCLUDE_PATH=${ADSIM_STAGING_DIR}/include -DTHRIFT_INCLUDE_DIRECTORIES=${ADSIM_STAGING_DIR}/include -DPYTHON_EXTENSIONS=OFF -Dthriftpy3_FOUND=OFF"

# Fix fb303 header installation paths
# OSS fb303 installs headers to fb303/thrift/gen-cpp2/
# AdSim's fb303.thrift wrapper will generate code that uses these directly
# We just need to ensure the staging include path has access to these headers
echo "Setting up fb303 header paths..."

FB303_SRC_DIR="${ADSIM_STAGING_DIR}/include/fb303/thrift/gen-cpp2"

if [ -d "$FB303_SRC_DIR" ] && [ -f "$FB303_SRC_DIR/fb303_core_types.h" ]; then
    echo "fb303 OSS headers found at $FB303_SRC_DIR"
    ls -la "$FB303_SRC_DIR/" | head -10
else
    echo "ERROR: fb303 gen-cpp2 headers not found at $FB303_SRC_DIR"
    echo "fb303 build may have failed to generate thrift code."
    find "${ADSIM_STAGING_DIR}" -name "*fb303*.h" 2>/dev/null | head -20
    exit 1
fi

# Build FBGEMM (Facebook General Matrix Multiplication) for ML workload simulation
./install_fbgemm.sh

echo ""
echo -e "${COLOR_GREEN}====================================================================="
echo "/Dependency build completed!"
echo "====================================================================="
echo "All dependencies have been built and installed to ${ADSIM_STAGING_DIR}/"
echo "Available directories: $(ls -1 ${ADSIM_STAGING_DIR}/ | grep -E '^(lib|include|lib64|bin|share)$' | tr '\n' ' ')"
echo -e "=====================================================================${COLOR_OFF}"
