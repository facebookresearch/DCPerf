#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### BENCHMARK CONFIG #########################

declare -A REPOS=(
    ['folly']='https://github.com/facebook/folly.git'
    ['fbthrift']='https://github.com/facebook/fbthrift.git'
    ['lzbench']='https://github.com/inikep/lzbench.git'
    ['openssl']='https://github.com/openssl/openssl.git'
)

declare -A TAGS=(
    ['folly']='v2024.12.09.00'
    ['fbthrift']='v2024.12.09.00'
    ['lzbench']='d138844ea56b36ff1c1c43b259c866069deb64ad'
    ['openssl']='openssl-3.3.1'
)

declare -A DATASETS=(
    ['silesia']='https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip'
)



##################### SYS CONFIG AND DEPS #########################

BPKGS_WDL_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_WDL_ROOT/../..")"
WDL_ROOT="${BENCHPRESS_ROOT}/benchmarks/wdl_bench"
WDL_SOURCE="${WDL_ROOT}/wdl_sources"
WDL_BUILD="${WDL_ROOT}/wdl_build"
WDL_DATASETS="${WDL_ROOT}/datasets"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  sudo apt install -y cmake autoconf automake flex bison \
    nasm clang patch git \
    tar unzip perl

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  sudo dnf install -y cmake autoconf automake flex bison \
    meson nasm clang patch \
    git tar unzip perl
fi


mkdir -p "${WDL_SOURCE}"
mkdir -p "${WDL_BUILD}"
mkdir -p "${WDL_DATASETS}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    sudo ln -s /usr/bin/cmake /usr/local/bin/cmake
fi

##################### BUILD AND INSTALL FUNCTIONS #########################

clone()
{
    lib=$1
    repo=${REPOS[$lib]}
    if ! git clone "${repo}" "${lib}" 2>/dev/null && [ -d "${lib}" ]; then
        echo "Clone failed because the folder ${lib} exists"
        return 1
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
    git apply "${BPKGS_WDL_ROOT}/0001-folly.patch"

    sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive

    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build --scratch-path "${WDL_BUILD}"

    popd || exit
}


build_fbthrift()
{
    lib='fbthrift'
    pushd "${WDL_SOURCE}"
    clone "$lib" || echo "Failed to clone $lib"
    cd "$lib" || exit

    sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive fbthrift

    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build fbthrift --scratch-path "${WDL_BUILD}"

    popd || exit
}


build_lzbench()
{
    lib='lzbench'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    make -j
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
    ./Configure --prefix="${WDL_BUILD}/openssl" --openssldir="${WDL_BUILD}/openssl"
    make -j
    make install
    cp "${WDL_BUILD}/openssl/bin/openssl" "${WDL_ROOT}/" || exit


    popd || exit
}


##################### BUILD AND INSTALL #########################

pushd "${WDL_ROOT}"

build_folly
build_fbthrift
build_lzbench
build_openssl

folly_benchmark_list="concurrency_concurrent_hash_map_bench hash_hash_benchmark hash_maps_bench stats_digest_builder_benchmark fibers_fibers_benchmark lt_hash_benchmark memcpy_benchmark memset_benchmark event_base_benchmark iobuf_benchmark function_benchmark random_benchmark small_locks_benchmark range_find_benchmark"

fbthrift_benchmark_list="ProtocolBench"

for benchmark in $folly_benchmark_list; do
  cp "$WDL_BUILD/build/folly/$benchmark" "$WDL_ROOT/$benchmark"
done

for benchmark in $fbthrift_benchmark_list; do
  cp "$WDL_BUILD/build/fbthrift/bin/$benchmark" "$WDL_ROOT/$benchmark"
done


cp "${BPKGS_WDL_ROOT}/run.sh" ./
cp "${BPKGS_WDL_ROOT}/convert.py" ./
cp "${BPKGS_WDL_ROOT}/aggregate_result.py" ./
cp "${BPKGS_WDL_ROOT}/parse_line.py" ./


popd

exit $?
