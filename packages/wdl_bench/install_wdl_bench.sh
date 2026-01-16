#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

GLIBC_VERSION=$(getconf GNU_LIBC_VERSION | cut -f 2 -d\  )

##################### BENCHMARK CONFIG #########################

declare -A REPOS=(
    ['lzbench']='https://github.com/inikep/lzbench.git'
    ['openssl']='https://github.com/openssl/openssl.git'
    ['vdso']='https://github.com/leitao/debug.git'
    ['libaegis']='https://github.com/aegis-aead/libaegis.git'
    ['xxhash']='https://github.com/Cyan4973/xxHash.git'
    ['glibc']='https://sourceware.org/git/glibc.git'
    ['isa-l']='https://github.com/intel/isa-l.git'
    ['sleef']='https://github.com/shibatch/sleef.git'
    ['aocl']='https://github.com/amd/aocl.git'
    ['acl']='https://github.com/ARM-software/ComputeLibrary.git'
    ['onednn']='https://github.com/uxlfoundation/oneDNN.git'
)

declare -A TAGS=(
    ['lzbench']='v2.2'
    ['openssl']='openssl-3.6.0'
    ['vdso']='a90085a8e4e1e07a93cc45a68da246fa98a9f831'
    ['libaegis']='0.4.2'
    ['xxhash']='136cc1f8fe4d5ea62a7c16c8424d4fa5158f6d68'
    ['glibc']="glibc-${GLIBC_VERSION}"
    ['isa-l']='d36de972efc18f2e85ca182a8b6758ecc7da512b'
    ['sleef']='3.8'
    ['aocl']='AOCL-5.2'
    ['acl']='v52.7.0'
    ['onednn']='v3.10.2'
)

declare -A DATASETS=(
    ['silesia']='https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip'
)



##################### SYS CONFIG AND DEPS #########################

BPKGS_WDL_ROOT="$(dirname "$(readlink -f -- "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
WDL_ROOT="${BENCHPRESS_ROOT}/benchmarks/wdl_bench"

# shellcheck disable=SC1091
source "$BPKGS_WDL_ROOT"/common.sh

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  DEBIAN_FRONTEND=noninteractive apt install -y cmake autoconf automake \
    flex bison gfortran nasm clang gcc g++ patch git libssl-dev libc6-dev \
    tar unzip perl openssl python3-dev gawk libstdc++6 python3-numpy \
    glibc-source libbenchmark-dev environment-modules libopenblas-dev \
    pkg-config ninja-build libgtest-dev

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y cmake autoconf automake flex bison gfortran \
    meson nasm clang gcc g++ patch glibc-static libstdc++-static \
    git tar unzip perl openssl-devel python3-devel gawk python3-numpy \
    dnf-plugins-core rpm-build audit-libs-devel gd-devel gdb \
    libcap-devel libpng-devel libselinux-devel texinfo valgrind \
    google-benchmark-devel environment-modules openblas-devel pkg-config \
    ninja-build gtest-devel

fi

mkdir -p "${WDL_SOURCE}"
mkdir -p "${WDL_BUILD}"
mkdir -p "${WDL_DATASETS}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    ln -s /usr/bin/cmake /usr/local/bin/cmake
fi

if ! in_conda_env; then
    if ! has_real_conda; then
        if [ ! -f "${WDL_ROOT}/miniconda3/etc/profile.d/conda.sh" ]; then
            echo "Installing miniconda."
            mkdir -p "${WDL_ROOT}/miniconda3"
            if [ "$ARCH" = "aarch64" ]; then
                wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-aarch64.sh -O "${WDL_ROOT}/miniconda3/miniconda.sh" || exit
            else
                wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O "${WDL_ROOT}/miniconda3/miniconda.sh" || exit
            fi
            bash -c "sh ${WDL_ROOT}/miniconda3/miniconda.sh -b -u -p ${WDL_ROOT}/miniconda3" || exit
            # shellcheck disable=SC1091
            source "${WDL_ROOT}"/miniconda3/etc/profile.d/conda.sh
            conda tos accept
        fi
    fi
fi

##################### BUILD AND INSTALL FUNCTIONS #########################

folly_benchmark_list="concurrency_concurrent_hash_map_bench hash_hash_benchmark container_hash_maps_bench stats_digest_builder_benchmark fibers_fibers_benchmark crypto_lt_hash_benchmark memcpy_benchmark memset_benchmark io_async_event_base_benchmark io_iobuf_benchmark function_benchmark random_benchmark synchronization_small_locks_benchmark synchronization_lifo_sem_bench range_find_benchmark hash_checksum_benchmark"

fbthrift_benchmark_list="ProtocolBench VarintUtilsBench"

# Pin Facebook library versions for reproducible builds
# Note: Facebook OSS libraries are released together with matching version tags
FOLLY_VERSION=v2026.01.05.00
FIZZ_VERSION=v2026.01.05.00
WANGLE_VERSION=v2026.01.05.00
MVFST_VERSION=v2026.01.05.00
FBTHRIFT_VERSION=v2026.01.05.00

# Staging directory for installed dependencies
STAGING_DIR="${WDL_BUILD}/installed"
DEPS_DIR="${WDL_BUILD}/deps"


clone()
{
    lib=$1
    repo=${REPOS[$lib]}
    if ! git clone "${repo}" "${lib}" 2>/dev/null && [ -d "${lib}" ]; then
        echo "Clone failed because the folder ${lib} exists"
        return 1
    fi
    if [ ! -d "${lib}" ]; then
        echo "Failed to clone ${lib} and directory does not exist."
        exit 1
    fi
    pushd "$lib" || exit 1
    tag=${TAGS[$lib]}
    git checkout "$tag" || exit 1
    popd || exit 1
}

download_dataset()
{
    dataset="$1"
    pushd "${WDL_DATASETS}"
    link=${DATASETS[$dataset]}
    wget "${link}" || exit 1

    popd || exit
}


# Generic function to build Facebook C++ dependencies using CMAKE
# Arguments:
#   $1: dep_name - Name of the dependency (e.g., "folly", "fbthrift")
#   $2: repo_url - Git repository URL
#   $3: version - Git tag/version to checkout (optional, uses default branch if empty)
#   $4: cmake_source_subdir - (Optional) Subdirectory containing CMakeLists.txt, defaults to "."
#   $5: extra_cmake_args - (Optional) Additional cmake arguments
build_dependency()
{
    local dep_name="$1"
    local repo_url="$2"
    local version="${3:-}"
    local cmake_source_subdir="${4:-.}"
    local extra_cmake_args="${5:-}"

    echo ""
    echo "====================================================================="
    echo "Building and Installing ${dep_name}"
    echo "====================================================================="

    local DEP_DIR="${DEPS_DIR}/${dep_name}"
    local BUILD_DIR="${DEP_DIR}/_build"

    # Clone repository if not already present (use --recursive to get submodules)
    if [ ! -d "$DEP_DIR" ]; then
        echo "Cloning ${dep_name} repo..."
        git clone --recursive "${repo_url}" "$DEP_DIR"
    fi

    cd "$DEP_DIR" || exit

    # Only checkout specific version if provided
    if [ -n "${version}" ]; then
        git fetch --tags
        git checkout "${version}"
        # Re-initialize submodules after checkout (needed for folly's build/fbcode_builder)
        git submodule update --init --recursive
    fi

    echo "Building ${dep_name}..."
    rm -rf "$BUILD_DIR"
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

    # Configure with CMAKE
    # shellcheck disable=SC2086
    cmake \
        -DCMAKE_PREFIX_PATH="${STAGING_DIR}" \
        -DCMAKE_INSTALL_PREFIX="${STAGING_DIR}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=20 \
        -DBUILD_TESTS=OFF \
        $extra_cmake_args \
        "${cmake_source_path}"

    local cmake_status="$?"
    if [ "$cmake_status" -ne 0 ]; then
        echo "CMAKE configuration for ${dep_name} failed!"
        exit $cmake_status
    fi

    # Build with make
    make -j "$JOBS"

    local build_status="$?"
    if [ "$build_status" -ne 0 ]; then
        echo "${dep_name} build failed!"
        exit $build_status
    fi

    # Install
    make install

    local install_status="$?"
    if [ "$install_status" -eq 0 ]; then
        echo "${dep_name} is installed"
    else
        echo "${dep_name} install failed!"
        exit $install_status
    fi

    cd "${WDL_BUILD}" || exit
}


build_folly()
{
    mkdir -p "${DEPS_DIR}"

    # Build fmt first as prerequisite
    build_dependency "fmt" "https://github.com/fmtlib/fmt.git" "" "." "-DFMT_DOC=OFF -DFMT_TEST=OFF"

    # Build folly
    build_dependency "folly" "https://github.com/facebook/folly.git" "${FOLLY_VERSION}"

    # Copy benchmarks
    for benchmark in $folly_benchmark_list; do
      if [ -f "${DEPS_DIR}/folly/_build/$benchmark" ]; then
        cp "${DEPS_DIR}/folly/_build/$benchmark" "$WDL_ROOT/$benchmark"
      elif [ -f "${DEPS_DIR}/folly/_build/folly/$benchmark" ]; then
        cp "${DEPS_DIR}/folly/_build/folly/$benchmark" "$WDL_ROOT/$benchmark"
      else
        echo "Warning: Could not find benchmark $benchmark"
      fi
    done
}


build_fbthrift()
{
    mkdir -p "${DEPS_DIR}"

    # Build all prerequisites in order
    build_dependency "fmt" "https://github.com/fmtlib/fmt.git" "" "." "-DFMT_DOC=OFF -DFMT_TEST=OFF"
    build_dependency "folly" "https://github.com/facebook/folly.git" "${FOLLY_VERSION}"
    build_dependency "fizz" "https://github.com/facebookincubator/fizz.git" "${FIZZ_VERSION}" "fizz"
    build_dependency "wangle" "https://github.com/facebook/wangle.git" "${WANGLE_VERSION}" "wangle"
    build_dependency "mvfst" "https://github.com/facebook/mvfst.git" "${MVFST_VERSION}"
    build_dependency "fbthrift" "https://github.com/facebook/fbthrift.git" "${FBTHRIFT_VERSION}"

    # Copy benchmarks
    for benchmark in $fbthrift_benchmark_list; do
      if [ -f "${DEPS_DIR}/fbthrift/_build/bin/$benchmark" ]; then
        cp "${DEPS_DIR}/fbthrift/_build/bin/$benchmark" "$WDL_ROOT/$benchmark"
      elif [ -f "${DEPS_DIR}/fbthrift/_build/$benchmark" ]; then
        cp "${DEPS_DIR}/fbthrift/_build/$benchmark" "$WDL_ROOT/$benchmark"
      else
        echo "Warning: Could not find benchmark $benchmark"
      fi
    done
}


build_lzbench()
{
    lib='lzbench'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    make BUILD_STATIC=1 -j "$JOBS"
    cp ./lzbench "${WDL_ROOT}/" || exit

    download_dataset 'silesia'
    pushd "${WDL_DATASETS}"
    unzip ./silesia.zip || exit
    rm  ./silesia.zip
    tar cvf silesia.tar ./*
    popd || exit

    popd || exit
}

build_openssl()
{
    lib='openssl'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    ./Configure no-docs --prefix="${WDL_BUILD}/openssl" --openssldir="${WDL_BUILD}/openssl"
    make -j "$JOBS"
    make install
    cp "${WDL_BUILD}/openssl/bin/openssl" "${WDL_ROOT}/" || exit

    popd || exit
}

build_vdso()
{
    lib='vdso'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib/vdso_bench" || exit
    make -j "$JOBS"
    cp ./vdso_bench "${WDL_ROOT}/" || exit

    popd || exit
}

build_libaegis()
{
    lib='libaegis'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    if [ "$ARCH" = "aarch64" ]; then
        wget https://ziglang.org/download/0.15.2/zig-aarch64-linux-0.15.2.tar.xz
        tar xvf zig-aarch64-linux-0.15.2.tar.xz
        mv zig-aarch64-linux-0.15.2 zig
    else
        wget https://ziglang.org/download/0.15.2/zig-x86_64-linux-0.15.2.tar.xz
        tar xvf zig-x86_64-linux-0.15.2.tar.xz
        mv zig-x86_64-linux-0.15.2 zig
    fi
    cd "$lib" || exit
    ../zig/zig build -Drelease -Dfavor-performance -Dwith-benchmark
    cp ./zig-out/bin/benchmark "${WDL_ROOT}/libaegis_benchmark" || exit

    popd || exit
}

build_xxhash()
{
    lib='xxhash'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    make -C ./tests/bench/ -j "$JOBS"
    cp ./tests/bench/benchHash "${WDL_ROOT}/xxhash_benchmark" || exit

    popd || exit
}

build_glibc()
{
    lib='glibc'
    pushd "${WDL_SOURCE}"
    if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
        # Ubuntu may tweak some glibc configurations that may break build,
        # so we should not use the official glibc. Instead, we extract the
        # glibc source code from the glibc-source package under /usr/src/glibc/
        mkdir "$lib"
        tar -xJf /usr/src/glibc/glibc-"${GLIBC_VERSION}".tar.xz -C "$lib" --strip-components=1
    elif [ "$LINUX_DIST_ID" = "centos" ]; then
        # Same as Ubuntu, centos may have private patches for glibc, so we rely
        # on the source rpm instead of the official glibc source code.
        # Extract Version and Release from installed glibc
        GLIBC_SPKG=$(rpm -qi glibc 2>/dev/null | awk -F': ' '
            /^Source RPM/ {
                sub(/\.rpm$/, "", $2)
                print $2
            }
            ')
        CENTOS_MAJOR="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"
        BASE_URL="https://mirror.stream.centos.org/${CENTOS_MAJOR}-stream/BaseOS/source/tree/"
        if dnf download --source "$GLIBC_SPKG" --setopt=timeout=60 >/dev/null 2>&1; then
            echo "${GLIBC_SPKG}.rpm downloaded from the default repo"
        elif dnf download --source "$GLIBC_SPKG" --repofrompath=src,"${BASE_URL}" --enablerepo=src --setopt=timeout=60 >/dev/null 2>&1; then
            echo "${GLIBC_SPKG}.rpm downloaded from the official CentOS repo"
        elif curl -fLO "${BASE_URL}/Packages/${GLIBC_SPKG}.rpm" --connect-timeout 60 >/dev/null 2>&1 && rpm -K "${GLIBC_SPKG}.rpm" >/dev/null 2>&1; then
            echo "${GLIBC_SPKG}.rpm downloaded from the official CentOS rpm package URL"
        else
            echo "Failed to download ${GLIBC_SPKG}.rpm"
            exit 1
        fi

        rpm -ivh "$lib"-*.src.rpm --define "_topdir ${WDL_SOURCE}/$lib-rpm"
        # N.B.: Do not change to rpmbuild because it might point to a different (private) binary that does not work.
        /usr/bin/rpmbuild -bp "./$lib-rpm/SPECS/glibc.spec" --define "_topdir ${WDL_SOURCE}/$lib-rpm"
        mv "${WDL_SOURCE}/$lib-rpm/BUILD/$lib-${GLIBC_VERSION}" "${WDL_SOURCE}/$lib"
    else
        clone $lib || echo "Failed to clone $lib"
    fi

    pushd "${WDL_BUILD}"
    mkdir glibc-build && cd glibc-build
    "${WDL_SOURCE}/$lib"/configure --prefix="${WDL_BUILD}/glibc-build"
    make -j "$JOBS"
    make bench-build -j "$JOBS"

    popd || exit
    popd || exit
}

build_isa_l()
{
    lib='isa-l'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    ./autogen.sh
    ./configure
    make perfs -j
    cp ./erasure_code/erasure_code_perf "${WDL_ROOT}/" || exit
    cp -r ./erasure_code/.libs "${WDL_ROOT}/" || exit

    popd || exit
}

build_sleef()
{
    lib='sleef'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    # Please do not change tabs in the following patch to spaces because git apply
    # is very sensitive to tabs and spaces.
    # @lint-ignore-section TXT2 on
    git apply - << 'EOF'
diff --git a/src/libm-benchmarks/CMakeLists.txt b/src/libm-benchmarks/CMakeLists.txt
index 379e541..7e8895d 100644
--- a/src/libm-benchmarks/CMakeLists.txt
+++ b/src/libm-benchmarks/CMakeLists.txt
@@ -13,6 +13,7 @@ ExternalProject_Add(googlebenchmark
   CMAKE_ARGS -DBENCHMARK_DOWNLOAD_DEPENDENCIES=ON
              -DCMAKE_BUILD_TYPE=Release
              -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/googlebench
+             -DCMAKE_INSTALL_LIBDIR=lib
              -DBENCHMARK_ENABLE_GTEST_TESTS=OFF
 )
 include_directories(${CMAKE_BINARY_DIR}/googlebench/include)
@@ -56,4 +57,4 @@ if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
 	target_compile_options(benchsleef512 PRIVATE ${EXTRA_CFLAGS} "-mavx512f" "-DARCH_VECT_LEN=512")
 	target_link_libraries(benchsleef512 sleef ${GOOGLE_BENCH_LIBS})
 	add_dependencies(benchsleef512 googlebenchmark)
-endif()
\ No newline at end of file
+endif()
--
EOF
    # @lint-ignore-section TXT2 off
    mkdir build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release -DSLEEF_BUILD_BENCH=on ../
    make -j "$JOBS"
    # Copy benchsleef128
    cp "${WDL_SOURCE}/sleef/build/bin/benchsleef128" "${WDL_ROOT}/" || exit 1
    # Copy benchsleef256 if it exists
    if [ -f "${WDL_SOURCE}/sleef/build/bin/benchsleef256" ]; then
        cp "${WDL_SOURCE}/sleef/build/bin/benchsleef256" "${WDL_ROOT}/" || exit 1
    fi
    # Copy benchsleef512 if it exists
    if [ -f "${WDL_SOURCE}/sleef/build/bin/benchsleef512" ]; then
        cp "${WDL_SOURCE}/sleef/build/bin/benchsleef512" "${WDL_ROOT}/" || exit 1
    fi

    popd || exit
}


build_stdcpp()
{
    lib='stdcpp_bench'
    pushd "$WDL_BUILD"
    cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build" -DCMAKE_BUILD_TYPE=Release && cmake --build "$lib-build"
    cp "$lib-build/$lib" "${WDL_ROOT}/" || exit 1
    popd || exit
}

build_gemm()
{
    lib='gemm_bench'
    pushd "$WDL_BUILD"
    source /etc/profile.d/modules.sh
    if [ "$ARCH" = "aarch64" ]; then
        clone acl || echo "Failed to clone acl"
        cd acl || exit
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DARM_COMPUTE_BUILD_SHARED_LIB=ON -DARM_COMPUTE_ENABLE_OPENMP=ON -DACL_MULTI_ISA=ON -DACL_BUILD_SVE=ON -DACL_BUILD_SVE2=ON -DACL_BUILD_SME2=ON -DCMAKE_INSTALL_PREFIX="${WDL_BUILD}/acl" && cmake --build build --config release --target install -- -j"$(nproc)"
        cd .. || exit
        if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
            wget -nc "https://developer.arm.com/-/cdn-downloads/permalink/Arm-Performance-Libraries/Version_${ARMPL_VERSION}/arm-performance-libraries_${ARMPL_VERSION}_deb_gcc.tar"
            tar xvf "arm-performance-libraries_${ARMPL_VERSION}_deb_gcc.tar"
            bash -c "./arm-performance-libraries_${ARMPL_VERSION}_deb/arm-performance-libraries_${ARMPL_VERSION}_deb.sh --accept --install-to ./apl"
        elif [ "$LINUX_DIST_ID" = "centos" ]; then
            wget -nc "https://developer.arm.com/-/cdn-downloads/permalink/Arm-Performance-Libraries/Version_${ARMPL_VERSION}/arm-performance-libraries_${ARMPL_VERSION}_rpm_gcc.tar"
            tar xvf "arm-performance-libraries_${ARMPL_VERSION}_rpm_gcc.tar"
            bash -c "./arm-performance-libraries_${ARMPL_VERSION}_rpm/arm-performance-libraries_${ARMPL_VERSION}_rpm.sh --accept --install-to ./apl"
        fi
        module use apl/modulefiles
        module load "armpl/${ARMPL_VERSION}_gcc"
        lib='gemm_bench' # reset lib to gemm_bench
        cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_RPATH="${ARMPL_LIBRARIES};${WDL_BUILD}/acl/lib64;${WDL_BUILD}/acl/lib" -DCMAKE_CXX_FLAGS="-I${WDL_BUILD}/acl/include -L ${WDL_BUILD}/acl/lib64 -L ${WDL_BUILD}/acl/lib" && cmake --build "$lib-build"
        cp "$lib-build/$lib" "${WDL_ROOT}/" || exit 1
    else
        cpu_vendor=$(grep -m 1 'vendor_id' /proc/cpuinfo | awk '{print $3}')
        wget -nc https://registrationcenter-download.intel.com/akdlm/IRC_NAS/2ad98b49-1fb2-4294-ab3d-6889b434ebd3/intel-onemkl-${MKL_VERSION}_offline.sh
        bash -c "sh ./intel-onemkl-${MKL_VERSION}_offline.sh -a --action install --silent --eula accept --install-dir ./onemkl"
        bash -c "onemkl/modulefiles-setup.sh --output-dir=onemkl/modulefiles"
        module use onemkl/modulefiles
        module load mkl/latest
        clone aocl || echo "Failed to clone aocl"
        cd aocl || exit
        # execute the build in a subshell with a new conda build environment
        (
            AOCL_BUILD_ENV="aocl_build_env"
            source_conda
            conda create --override-channels -y -c conda-forge --force -n "$AOCL_BUILD_ENV" "cmake>=3.26"
            conda activate "$AOCL_BUILD_ENV"
            cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_AOCL_BLAS=ON -DENABLE_AOCL_UTILS=ON -DENABLE_ADDON="aocl_gemm" -DENABLE_MULTITHREADING=ON -DCMAKE_INSTALL_PREFIX="${WDL_BUILD}/aocl"
            cmake --build build --config release --target install -- -j"$(nproc)"
            conda deactivate
            conda env remove -n "$AOCL_BUILD_ENV" -y
        )
        cd .. || exit
        lib='gemm_bench' # reset lib to gemm_bench
        cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build-onemkl" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_RPATH="${MKLROOT}/lib" && cmake --build "$lib-build-onemkl"
        cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build-aocl" -DCMAKE_BUILD_TYPE=Release -DCPU_VENDOR=AMD -DCMAKE_INSTALL_RPATH="${WDL_BUILD}/aocl/lib" -DCMAKE_CXX_FLAGS="-I${WDL_BUILD}/aocl/include -L ${WDL_BUILD}/aocl/lib" && cmake --build "$lib-build-aocl"
        if [[ "$cpu_vendor" == "GenuineIntel" ]]; then
            echo "CPU is Intel"
            cp "$lib-build-onemkl/$lib" "${WDL_ROOT}/" || exit 1
        elif [[ "$cpu_vendor" == "AuthenticAMD" ]]; then
            echo "CPU is AMD"
            cp "$lib-build-aocl/$lib" "${WDL_ROOT}/" || exit 1
        else
            echo "Unknown CPU vendor: $cpu_vendor"
        fi
    fi
    cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build-openblas" -DCMAKE_BUILD_TYPE=Release -DUSE_OPENBLAS=ON && cmake --build "$lib-build-openblas"
    clone onednn || echo "Failed to clone onednn"
    cd onednn || exit
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${WDL_BUILD}/onednn" && cmake --build build --config release --target install -- -j"$(nproc)"
    cd .. || exit
    lib='gemm_bench' # reset lib to gemm_bench
    cmake -S "$BPKGS_WDL_ROOT/$lib" -B "$lib-build-onednn" -DCMAKE_BUILD_TYPE=Release -DUSE_ONEDNN=ON -DCMAKE_INSTALL_RPATH="${WDL_BUILD}/onednn/lib;${WDL_BUILD}/onednn/lib64" -DCMAKE_CXX_FLAGS="-I${WDL_BUILD}/onednn/include -L ${WDL_BUILD}/onednn/lib -L ${WDL_BUILD}/onednn/lib64" && cmake --build "$lib-build-onednn"
    module purge

    popd || exit
}

##################### BUILD AND INSTALL #########################

pushd "${WDL_ROOT}"

TARGET=""
# Default JOBS from env var or nproc
# Use: NUM_BUILD_JOBS=16 ./benchpress -b wdl install prod_set
JOBS=${NUM_BUILD_JOBS:-$(nproc)}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)
            [[ -n "$2" ]] || { echo "Invalid option: $1 requires an argument"; exit 1; }
            TARGET="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--name <component>]"
            echo "  --name <component>  Build a specific component (default: all)"
            echo ""
            echo "Environment variables:"
            echo "  NUM_BUILD_JOBS      Number of parallel build jobs (default: nproc)"
            echo ""
            echo "Example: NUM_BUILD_JOBS=16 $0 --name folly"
            echo "If --name is omitted, ALL components will be built."
            exit 0
            ;;
        *)
            echo "Unsupported arg: $1"
            exit 1
            ;;
    esac
done

if [[ -z "$TARGET" ]]; then
    echo "No --name specified. Defaulting to building EVERYTHING."
    TARGET="all"
fi

case "$TARGET" in
    all)
        build_folly
        build_fbthrift
        build_lzbench
        build_openssl
        build_vdso
        build_libaegis
        build_xxhash
        build_glibc
        build_isa_l
        build_sleef
        build_stdcpp
        build_gemm
        ;;
    folly)    build_folly ;;
    fbthrift) build_fbthrift ;;
    lzbench)  build_lzbench ;;
    openssl)  build_openssl ;;
    vdso)     build_vdso ;;
    libaegis) build_libaegis ;;
    xxhash)   build_xxhash ;;
    glibc)    build_glibc ;;
    isa_l)    build_isa_l ;;
    sleef)    build_sleef ;;
    stdcpp)   build_stdcpp ;;
    gemm)  build_gemm ;;
    *)
        echo "Error: Unknown build target '$TARGET'"
        exit 1
        ;;
esac

# Ensure we're in WDL_ROOT before copying files
cd "${WDL_ROOT}" || exit 1
cp "${BPKGS_WDL_ROOT}/common.sh" ./
cp "${BPKGS_WDL_ROOT}/run.sh" ./
cp "${BPKGS_WDL_ROOT}/run_prod.sh" ./
cp "${BPKGS_WDL_ROOT}/convert.py" ./
cp "${BPKGS_WDL_ROOT}/aggregate_result.py" ./
cp "${BPKGS_WDL_ROOT}/parse_line.py" ./
cp "${BPKGS_WDL_ROOT}/scoring.py" ./
cp "${BPKGS_WDL_ROOT}/compare_results.py" ./

cp "${BPKGS_WDL_ROOT}/baseline_results" ./ -r


popd

exit $?
