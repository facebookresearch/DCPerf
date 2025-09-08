#!/bin/bash
# shellcheck disable=SC1091,SC2027,SC2086,SC2155,SC2010
# AdSim dependency builder - compiles Facebook C++ libraries and FBGEMM for ad simulation

# Cross-platform package installation function with package name mapping
install_packages() {
    # Detect OS distribution and install packages with appropriate names
    if command -v dnf >/dev/null 2>&1; then
        # Red Hat/Fedora/CentOS systems
        sudo dnf install -y clang-20.1.1 jemalloc-devel xxhash-devel bzip2-devel libomp-devel gengetopt gcc-toolset-14-libatomic-devel python3-devel
    elif command -v apt-get >/dev/null 2>&1; then
        # Ubuntu/Debian systems - map package names to Ubuntu equivalents
        sudo apt-get update
        sudo apt-get install -y clang libjemalloc-dev libxxhash-dev libbz2-dev libomp-dev gengetopt libatomic1-dev python3-dev
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
    local extra_build_args="$4"

    echo "Building ${dep_name}..."

    # Clone repository at specific version if not already present
    if [ ! -d "${ADSIM_DEPS_DIR}/${dep_name}" ]; then
        git clone "${repo_url}" "${ADSIM_DEPS_DIR}/${dep_name}" -b "${version}"
    fi

    # Apply fmt compatibility patch for build system
    patch -p0 -i "${ADSIM_PROJ_ROOT}/patches/fmt.patch" ${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/manifests/fmt

    # Apply libaegis manifest patch (creates new file)
    if [ ! -f "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/manifests/libaegis" ]; then
        patch -p1 -N -i "${ADSIM_PROJ_ROOT}/patches/libaegis-manifest.patch" -d "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/"
    fi

    # Apply fizz libaegis dependency patch
    patch -p1 -N -i "${ADSIM_PROJ_ROOT}/patches/fizz-libaegis.patch" -d "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/"

    # Build using Facebook's getdeps.py build system with C++20 standard
    pushd "${ADSIM_DEPS_DIR}/${dep_name}" || exit

    python3 "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/getdeps.py" install-system-deps --recursive

    python3 "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/getdeps.py" \
      --scratch-path="${ADSIM_STAGING_DIR}/getdeps-scratch" \
      --allow-system-packages \
      --extra-cmake-defines='{"CMAKE_C_COMPILER":"'${ADSIM_C_COMPILER}'","CMAKE_CXX_COMPILER":"'${ADSIM_CXX_COMPILER}'", "CMAKE_CXX_STANDARD":"20", "CMAKE_C_FLAGS":"-g1", "CMAKE_CXX_FLAGS":"-g1"}' \
      build "${dep_name}" --src-dir ${ADSIM_DEPS_DIR}/${dep_name} \
      --install-prefix="${ADSIM_STAGING_DIR}" \
      --install-dir="${ADSIM_STAGING_DIR}" \
      --verbose \
      $extra_build_args

    install_status="$?"

    echo "python3 "${ADSIM_DEPS_DIR}/${dep_name}/build/fbcode_builder/getdeps.py" \
      --scratch-path="${ADSIM_STAGING_DIR}/getdeps-scratch" \
      --allow-system-packages \
      --extra-cmake-defines='{\"CMAKE_C_COMPILER\":\"${ADSIM_C_COMPILER}\",\"CMAKE_CXX_COMPILER\":\"${ADSIM_CXX_COMPILER}\", \"CMAKE_CXX_STANDARD\":\"20\"}' \
      build "${dep_name}" --src-dir ${ADSIM_DEPS_DIR}/${dep_name} \
      --install-prefix="${ADSIM_STAGING_DIR}" \
      --install-dir="${ADSIM_STAGING_DIR}" \
      --verbose \
      $extra_build_args"
    popd || exit

    if [ "$install_status" -eq 0 ]; then
        echo "${dep_name} build completed successfully"
    else
        echo "${dep_name} build failed!"
        exit $install_status
    fi
}

# Build core Facebook C++ libraries: async runtime, RPC framework, monitoring
build_dependency "folly" "https://github.com/facebook/folly.git" "${FOLLY_VERSION}"
build_dependency "fbthrift" "https://github.com/facebook/fbthrift.git" "${FBTHRIFT_VERSION}"
build_dependency "fb303" "https://github.com/facebook/fb303.git" "${FB303_VERSION}" "--no-deps"

# Build FBGEMM (Facebook General Matrix Multiplication) for ML workload simulation
./install_fbgemm.sh

echo "Flattening dependency directories to staging root..."

# Function to create symbolic links for all directories and files
flatten_dependency() {
    local dep_dir="$1"
    local dep_name=$(basename "$dep_dir")

    if [ -d "$dep_dir" ]; then
        echo "Flattening ${dep_name}..."

        # Create symbolic links for all subdirectories and files
        for subdir in "$dep_dir"/*; do
            if [ -d "$subdir" ]; then
                local subdir_name=$(basename "$subdir")
                local target_dir="${ADSIM_STAGING_DIR}/${subdir_name}"
                echo "  Copying from from ${subdir_name} to ${target_dir}..."
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
# Flatten all dependency directories under staging, excluding specific ones
for item in "${ADSIM_STAGING_DIR}"/*; do
    if [ -d "$item" ]; then
        item_name=$(basename "$item")

        # Skip excluded directories
        case "$item_name" in
            bin|getdeps-scratch|lib|lib64|share)
                echo "Skipping excluded staging directory: ${item_name}"
                continue
                ;;
        esac

        flatten_dependency "$item"
    fi
done

echo "Dependency flattening completed!"
echo "All dependency subdirectories have been merged into ${ADSIM_STAGING_DIR}/"
echo "Available directories: $(ls -1 ${ADSIM_STAGING_DIR}/ | grep -E '^(lib|include|lib64|bin|share)$' | tr '\n' ' ')"
