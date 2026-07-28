#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -euo pipefail

# Path to the directory containing this script.
PKG_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${PKG_ROOT}/../..")"

# --- MULTICORE liblinear (OpenMP; `train -m <nr_thread>`) -------------------
# The multi-instance job (liblinear_multi) pins each instance to a core set and
# runs `train -m <threads>`, which only exists in cjlin's MULTICORE extension of
# liblinear (stock liblinear has no -m). We therefore build the multicore
# distribution here; it is a superset of stock train (accepts -s/-c/-B plus -m),
# so the single-instance liblinear_synthetic/liblinear_rcv1 jobs keep working.
#
# The multicore extension is distributed as a source archive (.zip) by cjlin at
# libsvmtools/multicore-liblinear. Pin the version here; override LIBLINEAR_MC_URL
# if the published path changes, or LIBLINEAR_MC_SRC to build from a local copy.
LIBLINEAR_MC_VERSION="${LIBLINEAR_MC_VERSION:-2.50}"
LIBLINEAR_MC_URL="${LIBLINEAR_MC_URL:-https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/multicore-liblinear/liblinear-multicore-${LIBLINEAR_MC_VERSION}.zip}"
# Optional escape hatch: a pre-placed local source dir or archive (dir with a
# Makefile, or a .tar.gz/.zip). When set, the download is skipped.
LIBLINEAR_MC_SRC="${LIBLINEAR_MC_SRC:-}"

# Default real dataset downloaded at install time (rcv1.binary, train + test).
LIBSVM_DATASETS_URL="https://www.csie.ntu.edu.tw/~cjlin/libsvmtools/datasets/binary"
RCV1_TRAIN_BZ2="rcv1_train.binary.bz2"
RCV1_TEST_BZ2="rcv1_test.binary.bz2"

source "${BENCHPRESS_ROOT}/packages/common/os-distro.sh"

# Install build/runtime dependencies for both CentOS-like and Ubuntu-like distros.
# unzip is added for the multicore archive; libgomp/OpenMP ships with gcc.
if distro_is_like ubuntu; then
    apt -y update
    apt -y install gcc g++ make git wget bzip2 unzip python3
else
    dnf -y install gcc gcc-c++ make git wget bzip2 unzip python3
fi

# --- Build multicore liblinear train/predict --------------------------------
rm -rf "${PKG_ROOT}/build"
mkdir -p "${PKG_ROOT}/build"
pushd "${PKG_ROOT}/build" >/dev/null
    src_dir=""
    if [ -n "${LIBLINEAR_MC_SRC}" ]; then
        # Use a pre-placed source dir or archive.
        if [ -d "${LIBLINEAR_MC_SRC}" ]; then
            cp -r "${LIBLINEAR_MC_SRC}" liblinear-mc
            src_dir="liblinear-mc"
        else
            case "${LIBLINEAR_MC_SRC}" in
                *.zip)          unzip -q "${LIBLINEAR_MC_SRC}" ;;
                *.tar.gz|*.tgz) tar -xzf "${LIBLINEAR_MC_SRC}" ;;
                *) echo "ERROR: unrecognized LIBLINEAR_MC_SRC archive: ${LIBLINEAR_MC_SRC}" >&2; exit 1 ;;
            esac
            src_dir="$(find . -maxdepth 1 -type d -name 'liblinear*' | head -1)"
        fi
    else
        # Download the pinned multicore archive.
        archive="${LIBLINEAR_MC_URL##*/}"
        echo "Fetching multicore liblinear: ${LIBLINEAR_MC_URL}"
        wget -O "${archive}" "${LIBLINEAR_MC_URL}"
        case "${archive}" in
            *.zip)          unzip -q "${archive}" ;;
            *.tar.gz|*.tgz) tar -xzf "${archive}" ;;
            *) echo "ERROR: unrecognized multicore archive '${archive}' (expected .tar.gz or .zip)" >&2; exit 1 ;;
        esac
        src_dir="$(find . -maxdepth 1 -type d -name 'liblinear*' | head -1)"
    fi

    if [ -z "${src_dir}" ] || [ ! -f "${src_dir}/Makefile" ]; then
        echo "ERROR: multicore liblinear source not found after fetch/extract." >&2
        echo "       Set LIBLINEAR_MC_URL to the correct archive, or LIBLINEAR_MC_SRC" >&2
        echo "       to a local source dir/archive. (Stock liblinear lacks '-m'.)" >&2
        exit 1
    fi

    pushd "${src_dir}" >/dev/null
        make -j"${BP_CPUS:-$(nproc)}"
        install -m755 -D train "${PKG_ROOT}/bin/train"
        install -m755 -D predict "${PKG_ROOT}/bin/predict"
    popd >/dev/null
popd >/dev/null
rm -rf "${PKG_ROOT}/build"

# Sanity-check that the built train supports -m (i.e. this is the multicore build).
if ! "${PKG_ROOT}/bin/train" 2>&1 | grep -q -- "-m nr_thread"; then
    echo "WARNING: built train does not advertise '-m nr_thread'; multi-instance" >&2
    echo "         thread pinning (liblinear_multi -T) may be ignored." >&2
fi

# --- Build the synthetic dataset generator (gen_libsvm) ---------------------
# Used by generate_dataset.sh to pre-build large sparse LIBSVM datasets sized by
# a target GiB footprint (the liblinear_multi workload; -f dataset).
gcc -O2 -Wall -o "${PKG_ROOT}/bin/gen_libsvm" "${PKG_ROOT}/gen_libsvm.c"

# Make the on-demand helper scripts executable (shipped in the package).
chmod +x "${PKG_ROOT}/run-liblinear-multi.sh" "${PKG_ROOT}/generate_dataset.sh" 2>/dev/null || true

# --- Download the default real dataset (rcv1.binary) ------------------------
mkdir -p "${PKG_ROOT}/datasets"
pushd "${PKG_ROOT}/datasets" >/dev/null
    for f in "${RCV1_TRAIN_BZ2}" "${RCV1_TEST_BZ2}"; do
        out="${f%.bz2}"
        if [ ! -f "${out}" ]; then
            wget -O "${f}" "${LIBSVM_DATASETS_URL}/${f}"
            bunzip2 -f "${f}"
        fi
    done
popd >/dev/null

echo "liblinear (multicore ${LIBLINEAR_MC_VERSION}) installed to ${PKG_ROOT}/bin"
echo "gen_libsvm generator built at ${PKG_ROOT}/bin/gen_libsvm"
echo "default dataset (rcv1.binary) downloaded to ${PKG_ROOT}/datasets"
