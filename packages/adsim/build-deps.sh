#!/bin/bash
# shellcheck disable=SC1091,SC2027,SC2086,SC2155,SC2010
# AdSim dependency builder - compiles Facebook C++ libraries and FBGEMM for ad simulation

# Cross-platform package installation function with package name mapping
install_packages() {
    # Detect OS distribution and install packages with appropriate names
    if command -v dnf >/dev/null 2>&1; then
        # Red Hat/Fedora/CentOS systems
        sudo dnf install -y clang-20.1.1 jemalloc-devel xxhash-devel bzip2-devel libomp-devel gengetopt gcc-toolset-14-libatomic-devel
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu/Debian systems - map package names to Ubuntu equivalents
        sudo apt-get update
        sudo apt-get install -y clang libjemalloc-dev libxxhash-dev libbz2-dev libomp-dev gengetopt libatomic1-dev
    else
        echo "Error: No supported package manager found (dnf and apt-get)"
        exit 1
    fi
}

# Load build configuration and environment variables
source config.sh

# Install system dependencies: compiler, memory allocator, hashing, compression, OpenMP, CLI parser
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
FOLLY_VERSION=v2025.06.23.00
FBTHRIFT_VERSION=v2025.06.23.00
FB303_VERSION=v2025.06.23.00

# Generic function to build Facebook C++ dependencies using getdeps.py
build_dependency() {
    local dep_name="$1"
    local repo_url="$2"
    local version="$3"

    echo "Building ${dep_name}..."

    # Clone repository at specific version if not already present
    if [ ! -d "${ADSIM_DEPS_DIR}/${dep_name}" ]; then
        git clone "${repo_url}" "${ADSIM_DEPS_DIR}/${dep_name}" -b "${version}"
    fi

    # Apply fmt compatibility patch for build system
    patch -p0 -i "${ADSIM_PROJ_ROOT}/patches/fmt.patch" ${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/manifests/fmt

    # Build using Facebook's getdeps.py build system with C++20 standard
    pushd "${ADSIM_DEPS_DIR}/${dep_name}" || exit
    python3 "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/getdeps.py" \
      --scratch-path="${ADSIM_STAGING_DIR}/getdeps-scratch" \
      --allow-system-packages \
      --extra-cmake-defines='{"CMAKE_C_COMPILER":"'${ADSIM_C_COMPILER}'","CMAKE_CXX_COMPILER":"'${ADSIM_CXX_COMPILER}'", "CMAKE_CXX_STANDARD":"20"}' \
      build "${dep_name}" --src-dir ${ADSIM_DEPS_DIR}/${dep_name} \
      --install-prefix="${ADSIM_STAGING_DIR}" \
      --install-dir="${ADSIM_STAGING_DIR}" \
      --verbose

    echo "python3 "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/getdeps.py" \
      --scratch-path="${ADSIM_STAGING_DIR}/getdeps-scratch" \
      --allow-system-packages \
      --extra-cmake-defines='{\"CMAKE_C_COMPILER\":\"${ADSIM_C_COMPILER}\",\"CMAKE_CXX_COMPILER\":\"${ADSIM_CXX_COMPILER}\", \"CMAKE_CXX_STANDARD\":\"20\"}' \
      build "${dep_name}" --src-dir ${ADSIM_DEPS_DIR}/${dep_name} \
      --install-prefix="${ADSIM_STAGING_DIR}" \
      --install-dir="${ADSIM_STAGING_DIR}" \
      --verbose"
    popd || exit

    echo "${dep_name} build completed successfully"
}

# Build core Facebook C++ libraries: async runtime, RPC framework, monitoring
build_dependency "folly" "https://github.com/facebook/folly.git" "${FOLLY_VERSION}"
build_dependency "fbthrift" "https://github.com/facebook/fbthrift.git" "${FBTHRIFT_VERSION}"
build_dependency "fb303" "https://github.com/facebook/fb303.git" "${FB303_VERSION}"

# Build FBGEMM (Facebook General Matrix Multiplication) for ML workload simulation
./install_fbgemm.sh

echo "Flattening dependency directories to staging root..."

# Function to merge dependency subdirectories into unified staging structure
flatten_dependency() {
    local dep_dir="$1"
    local dep_name=$(basename "$dep_dir")

    if [ -d "$dep_dir" ]; then
        echo "Flattening ${dep_name}..."

        # Merge lib/, include/, bin/ subdirectories into staging root
        for subdir in "$dep_dir"/*; do
            if [ -d "$subdir" ]; then
                local subdir_name=$(basename "$subdir")
                local target_dir="${ADSIM_STAGING_DIR}/${subdir_name}"

                echo "  Copying ${subdir_name} from ${dep_name}..."

                # Create target directory and merge contents
                mkdir -p "$target_dir"
                cp -r "$subdir"/* "$target_dir"/ 2>/dev/null || true
            fi
        done

        echo "  Completed flattening ${dep_name}"
    else
        echo "Warning: Directory ${dep_dir} not found"
    fi
}

# Flatten hash-named dependency directories (boost, fmt, glog, etc.)
for hash_dir in "${ADSIM_STAGING_DIR}"/boost-* \
                "${ADSIM_STAGING_DIR}"/fmt-* \
                "${ADSIM_STAGING_DIR}"/glog-* \
                "${ADSIM_STAGING_DIR}"/gflags-* \
                "${ADSIM_STAGING_DIR}"/googletest-* \
                "${ADSIM_STAGING_DIR}"/fast_float-* \
                "${ADSIM_STAGING_DIR}"/liboqs-*; do
    if [ -d "$hash_dir" ]; then
        flatten_dependency "$hash_dir"
    fi
done

# Flatten Facebook library directories (fbgemm, fb303, fbthrift, folly, etc.)
for dep_dir in "${ADSIM_STAGING_DIR}"/fbgemm \
               "${ADSIM_STAGING_DIR}"/fb303 \
               "${ADSIM_STAGING_DIR}"/fbthrift \
               "${ADSIM_STAGING_DIR}"/folly \
               "${ADSIM_STAGING_DIR}"/fizz \
               "${ADSIM_STAGING_DIR}"/wangle \
               "${ADSIM_STAGING_DIR}"/mvfst; do
    if [ -d "$dep_dir" ]; then
        flatten_dependency "$dep_dir"
    fi
done

echo "Dependency flattening completed!"
echo "All dependency subdirectories have been merged into ${ADSIM_STAGING_DIR}/"
echo "Available directories: $(ls -1 ${ADSIM_STAGING_DIR}/ | grep -E '^(lib|include|lib64|bin|share)$' | tr '\n' ' ')"
