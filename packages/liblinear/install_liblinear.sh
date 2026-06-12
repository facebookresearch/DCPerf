#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -euo pipefail

# Path to the directory containing this script.
PKG_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${PKG_ROOT}/../..")"

# Pinned upstream release of liblinear (https://github.com/cjlin1/liblinear).
# Bump this tag (and the README) to upgrade the benchmarked version.
LIBLINEAR_REPO="https://github.com/cjlin1/liblinear"
LIBLINEAR_TAG="v249"

# Default real dataset downloaded at install time (rcv1.binary, train + test).
LIBSVM_DATASETS_URL="https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary"
RCV1_TRAIN_BZ2="rcv1_train.binary.bz2"
RCV1_TEST_BZ2="rcv1_test.binary.bz2"

source "${BENCHPRESS_ROOT}/packages/common/os-distro.sh"

# Install build/runtime dependencies for both CentOS-like and Ubuntu-like distros.
if distro_is_like ubuntu; then
    apt -y update
    apt -y install gcc g++ make git wget bzip2 python3
else
    dnf -y install gcc gcc-c++ make git wget bzip2 python3
fi

# Build liblinear's train/predict CLIs from the pinned upstream source.
rm -rf "${PKG_ROOT}/build"
mkdir -p "${PKG_ROOT}/build"
pushd "${PKG_ROOT}/build"
    git clone "${LIBLINEAR_REPO}" liblinear
    pushd liblinear
        git checkout "tags/${LIBLINEAR_TAG}"
        make -j"${BP_CPUS:-$(nproc)}"
        install -m755 -D train "${PKG_ROOT}/bin/train"
        install -m755 -D predict "${PKG_ROOT}/bin/predict"
    popd
popd
rm -rf "${PKG_ROOT}/build"

# Download the default real dataset (rcv1.binary). Heavier sets stay opt-in.
mkdir -p "${PKG_ROOT}/datasets"
pushd "${PKG_ROOT}/datasets"
    for f in "${RCV1_TRAIN_BZ2}" "${RCV1_TEST_BZ2}"; do
        out="${f%.bz2}"
        if [ ! -f "${out}" ]; then
            wget -O "${f}" "${LIBSVM_DATASETS_URL}/${f}"
            bunzip2 -f "${f}"
        fi
    done
popd

echo "liblinear ${LIBLINEAR_TAG} installed to ${PKG_ROOT}/bin"
echo "default dataset (rcv1.binary) downloaded to ${PKG_ROOT}/datasets"
