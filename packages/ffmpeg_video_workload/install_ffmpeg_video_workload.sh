#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

##################### BENCHMARK CONFIG #########################

declare -A REPOS=(
    ['aom']='https://aomedia.googlesource.com/aom'
    ['ffmpeg']='https://git.ffmpeg.org/ffmpeg.git'
    ['SVT-AV1']='https://gitlab.com/AOMediaCodec/SVT-AV1.git'
    ['vmaf']='https://github.com/Netflix/vmaf.git'
    ['aom-testing']='https://gitlab.com/AOMediaCodec/aom-testing.git'
)

declare -A TAGS=(
    ['aom']='v3.8.2'
    ['ffmpeg']='n7.0.1'
    ['SVT-AV1']='v2.1.2'
    ['vmaf']='v3.0.0'
    ['aom-testing']='master'
)

declare -A DATASETS=(
    ['elfuente']='http://download.opencontent.netflix.com.s3.amazonaws.com/ElFuente/Netflix_Boat_4096x2160_60fps_10bit_420.y4m'
    ['elfuente_footmarket']='http://download.opencontent.netflix.com.s3.amazonaws.com/ElFuente/Netflix_FoodMarket_4096x2160_60fps_10bit_420.y4m'
)

##################### SYS CONFIG AND DEPS #########################

BPKGS_FFMPEG_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
ARCH="$(uname -p)"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_FFMPEG_ROOT/../..")"
FFMPEG_ROOT="${BENCHPRESS_ROOT}/benchmarks/ffmpeg_video_workload"
FFMPEG_SOURCE="${FFMPEG_ROOT}/ffmpeg_sources"
FFMPEG_BUILD="${FFMPEG_ROOT}/ffmpeg_build"
FFMPEG_DATASETS="${FFMPEG_ROOT}/datasets"

# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  sudo apt install -y cmake autoconf automake flex bison \
    meson nasm clang patch git \
    python3-dev pkg-config time parallel
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  sudo dnf install -y cmake autoconf automake flex bison \
    meson nasm clang patch \
    git python3-devel time parallel
fi

mkdir -p "${FFMPEG_SOURCE}"
mkdir -p "${FFMPEG_BUILD}"
mkdir -p "${FFMPEG_DATASETS}"

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
    fi
    pushd "$lib" || exit
    tag=${TAGS[$lib]}
    git checkout "$tag"
    popd || exit
}

build_svtav1()
{
    lib='SVT-AV1'
    pushd "${FFMPEG_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    mkdir -p _build && cd _build || exit
    cmake .. -G"Unix Makefiles" -DCMAKE_INSTALL_PREFIX="${FFMPEG_BUILD}"  -DBUILD_SHARED_LIBS=off -DCMAKE_BUILD_TYPE=Release
    make -j "$(nproc)" && make install
    popd
}

build_aom()
{
    lib='aom'
    pushd "${FFMPEG_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    mkdir -p _build && cd _build || exit
    if [ "$ARCH" = "x86_64" ]; then
        cmake .. -G"Unix Makefiles" -DCMAKE_INSTALL_PREFIX="${FFMPEG_BUILD}"  -DBUILD_SHARED_LIBS=off -DCMAKE_BUILD_TYPE=Release
    else
        cmake .. -G"Unix Makefiles" -DCMAKE_INSTALL_PREFIX="${FFMPEG_BUILD}"  -DBUILD_SHARED_LIBS=off -DCMAKE_BUILD_TYPE=Release -DAOM_TARGET_CPU=arm64
    fi
    make -j "$(nproc)" && make install
    popd || exit
}

build_vmaf()
{
    lib='vmaf'
    pushd "${FFMPEG_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib/libvmaf" || exit
    meson setup _build --prefix="${FFMPEG_BUILD}" --default-library static --prefer-static --buildtype release
    ninja -vC _build install
    popd || exit
}

build_ffmpeg()
{
    pushd "${FFMPEG_SOURCE}"
    lib=ffmpeg
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    mkdir -p _build && cd _build || exit
    if [ -v PKG_CONFIG_PATH ]; then
        PKG_CONFIG_PATH=$PKG_CONFIG_PATH:$FFMPEG_BUILD/lib/pkgconfig:$FFMPEG_BUILD/lib64/pkgconfig \
            ../configure --ld="g++" \
            --enable-gpl --enable-nonfree --enable-version3 \
            --enable-static --disable-shared \
            --pkg-config-flags=--static \
            --cc="clang" \
            --cxx="clang++" \
            --enable-libaom \
            --enable-libsvtav1 \
            --enable-libvmaf \
            --extra-cflags="-I${FFMPEG_BUILD}/include " \
            --extra-cxxflags="-I${FFMPEG_BUILD}/include " \
            --extra-ldflags="-L${FFMPEG_BUILD}/lib" \
            --prefix="${FFMPEG_BUILD}"

    else
        PKG_CONFIG_PATH=$FFMPEG_BUILD/lib/pkgconfig:$FFMPEG_BUILD/lib64/pkgconfig \
            ../configure --ld="g++" \
            --enable-gpl --enable-nonfree --enable-version3 \
            --enable-static --disable-shared \
            --pkg-config-flags=--static \
            --cc="clang" \
            --cxx="clang++" \
            --enable-libaom \
            --enable-libsvtav1 \
            --enable-libvmaf \
            --extra-cflags="-I${FFMPEG_BUILD}/include " \
            --extra-cxxflags="-I${FFMPEG_BUILD}/include " \
            --extra-ldflags="-L${FFMPEG_BUILD}/lib" \
            --prefix="${FFMPEG_BUILD}"

    fi

    git apply "${BPKGS_FFMPEG_ROOT}/0001-ffmpeg.patch"
    make -j "$(nproc)" && make install
    popd || exit
}

download_dataset()
{
    dataset="$1"
    pushd "${FFMPEG_DATASETS}"
    link=${DATASETS[$dataset]}
    wget "${link}"

    popd || exit
}

download_testing_scripts()
{
    pushd "${FFMPEG_ROOT}"
    clone 'aom-testing'
    sed -i "s@/home/user/stream@${FFMPEG_DATASETS}@g" ./aom-testing/scripts/content-adaptive-streaming-pipeline-scripts/generate_commands_all.py
    sed -i '/^ENCODER/s/^/\#/' ./aom-testing/scripts/content-adaptive-streaming-pipeline-scripts/generate_commands_all.py
    sed -i '/^ENC_MODES/s/^/\#/' ./aom-testing/scripts/content-adaptive-streaming-pipeline-scripts/generate_commands_all.py
    popd || exit
}

##################### BUILD AND INSTALL #########################

pushd "${FFMPEG_ROOT}"

build_svtav1
build_aom
build_vmaf
build_ffmpeg

download_dataset 'elfuente'
#download_dataset 'elfuente_footmarket'


download_testing_scripts
cp "${BPKGS_FFMPEG_ROOT}/run.sh" ./
cp ./aom-testing/scripts/content-adaptive-streaming-pipeline-scripts/generate_commands_all.py ./
mkdir -p tools
ln -s "${FFMPEG_BUILD}/bin/ffmpeg" ./tools/ffmpeg
ln -s "${FFMPEG_BUILD}/bin/ffmpeg" ./ffmpeg
popd

exit $?