#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

GLIBC_VERSION=$(getconf GNU_LIBC_VERSION | cut -f 2 -d\  )

##################### BENCHMARK CONFIG #########################

declare -A REPOS=(
    ['folly']='https://github.com/facebook/folly.git'
    ['fbthrift']='https://github.com/facebook/fbthrift.git'
    ['lzbench']='https://github.com/inikep/lzbench.git'
    ['openssl']='https://github.com/openssl/openssl.git'
    ['vdso']='https://github.com/leitao/debug.git'
    ['libaegis']='https://github.com/aegis-aead/libaegis.git'
    ['xxhash']='https://github.com/Cyan4973/xxHash.git'
    ['glibc']='https://sourceware.org/git/glibc.git'
    ['isa-l']='https://github.com/intel/isa-l.git'
    ['sleef']='https://github.com/shibatch/sleef.git'
)

declare -A TAGS=(
    ['folly']='v2025.11.17.00'
    ['fbthrift']='v2025.11.17.00'
    ['lzbench']='v2.2'
    ['openssl']='openssl-3.6.0'
    ['vdso']='a90085a8e4e1e07a93cc45a68da246fa98a9f831'
    ['libaegis']='0.4.2'
    ['xxhash']='136cc1f8fe4d5ea62a7c16c8424d4fa5158f6d68'
    ['glibc']="glibc-${GLIBC_VERSION}"
    ['isa-l']='d36de972efc18f2e85ca182a8b6758ecc7da512b'
    ['sleef']='3.8'
)

declare -A DATASETS=(
    ['silesia']='https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip'
)



##################### SYS CONFIG AND DEPS #########################

BPKGS_WDL_ROOT="$(dirname "$(readlink -f -- "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
WDL_ROOT="${BENCHPRESS_ROOT}/benchmarks/wdl_bench"
WDL_SOURCE="${WDL_ROOT}/wdl_sources"
WDL_BUILD="${WDL_ROOT}/wdl_build"
WDL_DATASETS="${WDL_ROOT}/datasets"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y cmake autoconf automake flex bison \
    nasm clang patch git libssl-dev libc6-dev\
    tar unzip perl openssl python3-dev gawk libstdc++6 python3-numpy \
    glibc-source libbenchmark-dev

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y cmake autoconf automake flex bison \
    meson nasm clang patch glibc-static libstdc++-static \
    git tar unzip perl openssl-devel python3-devel gawk python3-numpy \
    dnf-plugins-core rpm-build audit-libs-devel gd-devel gdb \
    libcap-devel libpng-devel libselinux-devel texinfo valgrind \
    google-benchmark-devel
fi


mkdir -p "${WDL_SOURCE}"
mkdir -p "${WDL_BUILD}"
mkdir -p "${WDL_DATASETS}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    ln -s /usr/bin/cmake /usr/local/bin/cmake
fi

##################### BUILD AND INSTALL FUNCTIONS #########################

folly_benchmark_list="concurrency_concurrent_hash_map_bench hash_hash_benchmark container_hash_maps_bench stats_digest_builder_benchmark fibers_fibers_benchmark crypto_lt_hash_benchmark memcpy_benchmark memset_benchmark io_async_event_base_benchmark io_iobuf_benchmark function_benchmark random_benchmark synchronization_small_locks_benchmark synchronization_lifo_sem_bench range_find_benchmark hash_checksum_benchmark"

fbthrift_benchmark_list="ProtocolBench VarintUtilsBench"


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



build_folly()
{
    lib='folly'
    pushd "${WDL_SOURCE}"
    clone "$lib" || echo "Failed to clone $lib"
    cd "$lib" || exit

    python3 ./build/fbcode_builder/getdeps.py install-system-deps --recursive

    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build --src-dir "." --scratch-path "${WDL_BUILD}"

    for benchmark in $folly_benchmark_list; do
      cp "$WDL_BUILD/build/folly/$benchmark" "$WDL_ROOT/$benchmark"
    done

    popd || exit
}


build_fbthrift()
{
    lib='fbthrift'
    pushd "${WDL_SOURCE}"
    clone "$lib" || echo "Failed to clone $lib"
    cd "$lib" || exit

    ./build/fbcode_builder/getdeps.py install-system-deps --recursive fbthrift

    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build fbthrift --src-dir "." --scratch-path "${WDL_BUILD}" --extra-cmake-defines='{"enable_tests": "1"}'

    for benchmark in $fbthrift_benchmark_list; do
      cp "$WDL_BUILD/build/fbthrift/bin/$benchmark" "$WDL_ROOT/$benchmark"
    done

    popd || exit
}


build_lzbench()
{
    lib='lzbench'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    make BUILD_STATIC=1 -j "$(nproc)"
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
    make -j "$(nproc)"
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
    make -j "$(nproc)"
    cp ./vdso_bench "${WDL_ROOT}/" || exit

    popd || exit
}

build_libaegis()
{
    lib='libaegis'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    ARCH="$(uname -p)"
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
    make -C ./tests/bench/ -j "$(nproc)"
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
    make -j "$(nproc)"
    make bench-build -j "$(nproc)"

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
    make -j "$(nproc)"
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

##################### BUILD AND INSTALL #########################

pushd "${WDL_ROOT}"

TARGET=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --name)
            [[ -n "$2" ]] || { echo "Invalid option: $1 requires an argument"; exit 1; }
            TARGET="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--name <component>]"
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
    *)
        echo "Error: Unknown build target '$TARGET'"
        exit 1
        ;;
esac

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
