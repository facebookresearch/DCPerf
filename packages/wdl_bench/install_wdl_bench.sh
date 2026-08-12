#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

# Newer OS/FW images ship CMake >= 4.0, which removed compatibility with
# cmake_minimum_required(VERSION < 3.5). Several pinned deps built here
# (e.g. gflags 2.2.2, glog 0.4.0 via folly getdeps; aom/ffmpeg CMake)
# still declare old minimums and abort with "Compatibility with CMake
# < 3.5 has been removed". Ask CMake to assume a 3.5 policy baseline for
# those old projects. Harmless / no-op on CMake 3.x.
export CMAKE_POLICY_VERSION_MINIMUM=3.5

##################### BENCHMARK CONFIG #########################

declare -A REPOS=(
    ['folly']='https://github.com/facebook/folly.git'
    ['fbthrift']='https://github.com/facebook/fbthrift.git'
    ['lzbench']='https://github.com/inikep/lzbench.git'
    ['openssl']='https://github.com/openssl/openssl.git'
)

declare -A TAGS=(
    ['folly']='v2025.05.12.00'
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
VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"

# CentOS containers run as root but with root locked and no sudo PAM
# setup until the workflow adds it — `sudo` fails there with
# "account validation failure, is your account locked?" on dnf installs.
SUDO=""
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  SUDO="sudo"
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  if sudo -n true 2>/dev/null; then
    SUDO="sudo"
  else
    SUDO=""
  fi
fi

if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  ${SUDO} apt install -y cmake autoconf automake flex bison \
    nasm clang patch git \
    tar unzip perl openssl python3-dev

elif [ "$LINUX_DIST_ID" = "centos" ]; then
  ${SUDO} dnf install -y cmake autoconf automake flex bison \
    meson nasm clang patch \
    git tar unzip perl openssl openssl-devel openssl-libs python3-devel
fi


mkdir -p "${WDL_SOURCE}"
mkdir -p "${WDL_BUILD}"
mkdir -p "${WDL_DATASETS}"

if ! [ -f "/usr/local/bin/cmake" ]; then
    ${SUDO} ln -s /usr/bin/cmake /usr/local/bin/cmake
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



# The folly/fbthrift clones bundle stale getdeps manifests that
# need a few fixups before the dep graph can build. Must run from inside the
# checked-out repo (cwd = the cloned source dir).
fixup_getdeps_manifests()
{
    # zlib.net/fossils is 503 behind fwdproxy on CentOS (P2460698823).
    # Use GitHub mirror (fwdproxy-allowed on rtpmachine) — same logic as
    # fbcode/cea/chips/benchpress/DCPerf/packages/tao_bench/install_tao_bench_aarch64.sh
    # Layout differs (zlib-1.3.1/ prefix), so also patch the sha from
    # 9a93b2b7... (zlib.net layout) to 17e88863... (github layout).
    sed -i -E 's#url = https://zlib\.net/(fossils/)?zlib-#url = https://github.com/madler/zlib/archive/refs/tags/v#' build/fbcode_builder/manifests/zlib
    sed -i 's#zlib-v#zlib-#' build/fbcode_builder/manifests/zlib 2>/dev/null || true
    sed -i 's/9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23/17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c/' build/fbcode_builder/manifests/zlib 2>/dev/null || true

    # Sandcastle's proxy can't reach ftp.gnu.org (503 tunnel), so libiberty's
    # binutils download fails there (works on open-egress dev hosts). Repoint at
    # GNU's mirror redirector (what current fbsource uses); host-only swap keeps
    # the pinned version + sha256.
    sed -i 's|https://ftp.gnu.org/gnu/binutils/|https://ftpmirror.gnu.org/gnu/binutils/|' build/fbcode_builder/manifests/libiberty

    # getdeps _apply_patchfile() runs `git apply` from the wrong cwd when a custom
    # --scratch-path is used, so every manifest patchfile fails. These patches are
    # Windows-only / minor and unneeded for the Linux benchmark build; drop them.
    sed -i '/^patchfile[[:space:]]*=/d' build/fbcode_builder/manifests/*
    rm -f build/fbcode_builder/patches/*

    # CentOS 10 dropped the lz4-static rpm (only lz4-devel remains); strip it so
    # `dnf install` doesn't abort. The build links the system dynamic lz4 via
    # --allow-system-packages.
    if [ "$LINUX_DIST_ID" = "centos" ] && [ "$VERSION_ID" -ge 10 ]; then
        sed -i '/^lz4-static$/d' build/fbcode_builder/manifests/lz4
    fi

    # wdl centos9/10 boost 1.75→1.83 double-imports every Boost::* target
    # (get_target_property ... IMPORTED_LOCATION_RELEASE already has
    # ${__boost_imploc} overwritten) — that hard SEND_ERROR breaks
    # folly's cmake configure for type_erasure, atomic, stacktrace, etc.
    # Downgrade all SEND_ERROR to WARNING so the build's IMPORTED_LOCATION
    # just warns and keeps going.
    if [ -d "build/fbcode_builder/manifests" ]; then
        sed -i 's/SEND_ERROR/WARNING/g' build/fbcode_builder/manifests/boost 2>/dev/null || true
        # Also patch the extracted boost BoostConfig.cmake that getdeps
        # writes under wdl_build — that file carries the same SEND_ERROR.
        find "${WDL_BUILD:-/tmp}" -name "BoostConfig.cmake" -exec sed -i 's/SEND_ERROR/WARNING/g' {} \; 2>/dev/null || true
        find . -name "BoostConfig.cmake" -exec sed -i 's/SEND_ERROR/WARNING/g' {} \; 2>/dev/null || true
    fi
}

# fbthrift @ v2024.12.09.00 ships a folly pin under build/deps/github_hashes but
# NOT one for its sibling Meta libs (fizz/wangle/mvfst). With no pin, getdeps
# fetches them from their default branch (see fetcher.py GitFetcher), so a
# current fizz `main` links folly's newer CMake target Folly::folly_range, which
# the pinned Dec-2024 folly doesn't export -> fizz configure fails. Pin any
# unpinned sibling to the SAME release tag as fbthrift so the set is consistent.
# No-op when a pin already exists (never override fbthrift's own pins). Must be
# called from inside the checked-out repo (cwd = the cloned source dir).
pin_getdeps_dep()
{
    local org="$1" repo="$2"
    local tag="${TAGS[fbthrift]}"
    local hashfile="build/deps/github_hashes/${org}/${repo}-rev.txt"
    if [ -f "$hashfile" ]; then
        return 0
    fi
    local refs sha
    refs="$(git ls-remote "https://github.com/${org}/${repo}.git" "refs/tags/${tag}" 2>/dev/null || true)"
    # Prefer the peeled commit (^{}) for annotated tags; fall back to the tag ref.
    sha="$(printf '%s\n' "$refs" | awk '/\}$/ {print $1; exit}')"
    if [ -z "$sha" ]; then
        sha="$(printf '%s\n' "$refs" | awk 'NR==1 {print $1}')"
    fi
    if [ -n "$sha" ]; then
        mkdir -p "build/deps/github_hashes/${org}"
        echo "Subproject commit ${sha}" > "$hashfile"
        echo "Pinned ${org}/${repo} to ${tag} (${sha})"
    else
        echo "WARNING: could not resolve ${org}/${repo} tag ${tag}; leaving unpinned"
    fi
}


build_folly()
{
    lib='folly'
    pushd "${WDL_SOURCE}"
    clone "$lib" || echo "Failed to clone $lib"
    cd "$lib" || exit
    git apply "${BPKGS_WDL_ROOT}/0001-folly.patch"
    fixup_getdeps_manifests
    # getdeps internally does `sudo dnf install -y autoconf ... zlib-static`;
    # on CentOS minimal the PAM account is locked until the workflow's
    # Fix PAM (pam_rootok/sudoers/passwd -u) runs — that sudo fails with
    # "account validation failure, is your account locked?".
    # Fall back to dnf without sudo when sudo is not usable (SUDO="").
    if [ -n "${SUDO}" ]; then
        ./build/fbcode_builder/getdeps.py install-system-deps --recursive \
          || sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive
    else
        ./build/fbcode_builder/getdeps.py --no-sudo install-system-deps --recursive 2>/dev/null \
          || ./build/fbcode_builder/getdeps.py install-system-deps --recursive \
          || dnf install -y autoconf automake binutils binutils-devel cmake double-conversion double-conversion-devel libdwarf libdwarf-devel libevent-devel libsodium-devel libtool libunwind libunwind-devel libzstd libzstd-devel lz4-devel ninja-build snappy-devel xz-devel zlib-devel || true
    fi

    # --src-dir "." builds folly from THIS checked-out tag instead of letting
    # getdeps re-fetch the project from its floating default branch.
    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build --src-dir "." --scratch-path "${WDL_BUILD}"

    popd || exit
}


build_fbthrift()
{
    lib='fbthrift'
    pushd "${WDL_SOURCE}"
    clone "$lib" || echo "Failed to clone $lib"
    cd "$lib" || exit
    fixup_getdeps_manifests
    # Pin unpinned sibling Meta libs so getdeps doesn't float them to a folly-
    # incompatible main (see pin_getdeps_dep above).
    pin_getdeps_dep facebookincubator fizz
    pin_getdeps_dep facebook wangle
    pin_getdeps_dep facebook mvfst

    if [ -n "${SUDO}" ]; then
        ${SUDO} ./build/fbcode_builder/getdeps.py install-system-deps --recursive fbthrift \
          || ./build/fbcode_builder/getdeps.py install-system-deps --recursive fbthrift
    else
        ./build/fbcode_builder/getdeps.py --no-sudo install-system-deps --recursive fbthrift 2>/dev/null \
          || ./build/fbcode_builder/getdeps.py install-system-deps --recursive fbthrift 2>/dev/null \
          || true
    fi

    # --src-dir "." builds fbthrift from THIS checked-out tag; without it getdeps
    # re-fetches fbthrift from main, whose newer source uses folly symbols
    # (FOLLY_PRAGMA_UNROLL_N, folly::available_concurrency) absent from the
    # pinned Dec-2024 folly.
    python3 ./build/fbcode_builder/getdeps.py --allow-system-packages build fbthrift --src-dir "." --scratch-path "${WDL_BUILD}"

    popd || exit
}


build_lzbench()
{
    lib='lzbench'
    pushd "${WDL_SOURCE}"
    clone $lib || echo "Failed to clone $lib"
    cd "$lib" || exit
    # GCC 14+ (CentOS 10 ships gcc 15) promotes -Wint-conversion and friends from
    # warnings to hard errors, which breaks lzbench's bundled legacy C (e.g.
    # glza/GLZAcompress.c casts a uint32_t* to atomic_uintptr_t). Keep them as
    # warnings so the pinned lzbench still builds. No-op on older GCC (el9).
    make -j CC="${CC:-cc} -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration"
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
