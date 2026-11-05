#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# Installs Silo's `dbtest` benchmark from https://github.com/stephentu/silo.
# Silo is an in-memory OLTP database from MIT (SOSP 2013); `dbtest` is a
# single binary that runs TPC-C, YCSB, and a handful of microbenchmarks
# (queue, bid, ic3). We build it in MODE=perf with USE_MALLOC_MODE=1 so
# the masstree variant is selected; the build output lands in
# `out-perf.masstree/benchmarks/dbtest`.
#
# We install into ${BENCHPRESS_ROOT}/benchmarks/silo/dbtest so the binary
# sits in its own subdirectory (matching the graph500/gapbs layout) and
# `install_markers` in benchmarks_mem.yml can detect a completed
# install.

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

# System deps. autoconf / automake / libtool are required because
# masstree's build runs `autoreconf -i`. zlib-devel is required for
# dbtest's `-lz` link step.
# `mysql-devel` conflicts with the preinstalled `fb-mysql-libs-universal`
# on AArch64 hosts, and Silo's embedded-MySQL wrapper is not used by any
# benchpress job (see MYSQL=0 below), so it is only installed on x86_64.
# `fb-fwdproxy-config` only exists inside Meta; install it only when dnf
# can see it, and use plain git on public builders (see GIT_PROXY_ARGS
# below). Git access through fwdproxy is only needed inside Meta.
if command -v dnf >/dev/null 2>&1 \
    && dnf info fb-fwdproxy-config >/dev/null 2>&1; then
  dnf install -y fb-fwdproxy-config
fi
DNF_DEPS=(
    autoconf
    automake
    libtool
    jemalloc-devel
    numactl-devel
    libdb-cxx-devel
    libaio-devel
    openssl-devel
    zlib-devel
)
ARCH="$(uname -m)"
if [[ "${ARCH}" != "aarch64" && "${ARCH}" != "arm64" ]]; then
    DNF_DEPS+=(mysql-devel)
fi
dnf install -y "${DNF_DEPS[@]}"

SILO_GIT_REPO_URL="https://github.com/stephentu/silo.git"
# Pinned to the last commit on stephentu/silo master as of the original
# benchpress import; bump deliberately and re-test if you need a newer rev.
SILO_GIT_COMMIT_TAG="cc11ca1ea949ef266ee12a9b1c310392519d9e3b"

BENCHMARKS_DIR="$(pwd)/benchmarks"
mkdir -p "$BENCHMARKS_DIR"

SILO_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/silo"
SILO_BINARY_PATH="${SILO_INSTALLATION_PREFIX}/dbtest"

# Silo's masstree submodule contains a `.so` that is dynamically linked at
# runtime, so we cannot delete the build tree after install. We do, however,
# wipe and recreate the build tree on each install so the install is
# repeatable.
rm -rf silo_build
mkdir -p silo_build
cd silo_build

"${BENCHPRESS_ROOT}"/install_remove_git.sh install

# Git through fwdproxy when the helper exists (internal hosts); plain
# git on public builders where it does not.
GIT_PROXY_ARGS=()
if command -v fwdproxy-config >/dev/null 2>&1; then
  read -ra GIT_PROXY_ARGS <<<"$(fwdproxy-config --git-command git)" || true
fi

git "${GIT_PROXY_ARGS[@]}" clone "$SILO_GIT_REPO_URL"
cd silo
git checkout -b benchpress "${SILO_GIT_COMMIT_TAG}"

# The masstree submodule's .gitmodules pins git:// URLs that github no
# longer serves. Rewrite to https:// before recursing.
sed -i 's|git://|https://|g' .gitmodules
git "${GIT_PROXY_ARGS[@]}" submodule update --init --recursive

# Apply the cgroup-cpuset-awareness + no-NUMA patch (authored against the pinned
# commit above). It (a) removes Silo's explicit NUMA memory hint so DB-pool
# placement is pure first-touch, and (b) adds a `dbtest --cgroup-aware` runtime
# flag (default off) that pins each worker to its inherited cgroup cpuset CPU.
# Default behavior (no flag) matches stock Silo. Applied to the freshly-cloned
# tree, so a plain check-then-apply is sufficient.
SILO_PATCH="${SCRIPT_DIR}/patches/silo-cgroup-aware-no-numa.patch"
git apply --check "${SILO_PATCH}" && git apply "${SILO_PATCH}"

# Pre-existing upstream bug, not AArch64-specific: the sequential YCSB
# loader drops the nkeys % nthreads remainder keys, which readers then
# fail to find (e.g. scale-factor 1 over 16 threads loads 992 of 1000
# keys and aborts). Give the last loader the remainder, as the parallel
# loaders already do. Applied on all architectures.
LOADER_PATCH="${SCRIPT_DIR}/patches/silo-ycsb-loader-remainder.patch"
git apply --check "${LOADER_PATCH}" && git apply "${LOADER_PATCH}"

# AArch64: Silo and Masstree contain x86-only inline asm (`pause`, `rdtsc`,
# `prefetcht0`, `bswapq`, `mfence`, `xchg`/`cmpxchg`). The patches below
# replace those with architected AArch64 equivalents (`yield`, `CNTVCT_EL0`,
# `__builtin_prefetch`, `__builtin_bswap64`, `dmb ish`, `__sync_*`
# builtins); every x86 code path is left unchanged behind its guard.
# Applied only on AArch64 so the x86_64 build is untouched. The Masstree
# patch lives in the submodule, so it is applied inside `masstree/`.
# On AArch64 the build also drops the embedded-MySQL wrapper (MYSQL=0):
# its headers/library are not installable there (see above) and no
# benchpress job runs the `mysql` bench.
DROP_MYSQL_WRAPPER=0
if [[ "${ARCH}" == "aarch64" || "${ARCH}" == "arm64" ]]; then
  SILO_AARCH64_PATCH="${SCRIPT_DIR}/patches/silo-aarch64.patch"
  git apply --check "${SILO_AARCH64_PATCH}" && git apply "${SILO_AARCH64_PATCH}"
  MASSTREE_AARCH64_PATCH="${SCRIPT_DIR}/patches/masstree-aarch64.patch"
  git -C masstree apply --check "${MASSTREE_AARCH64_PATCH}" \
    && git -C masstree apply "${MASSTREE_AARCH64_PATCH}"
  DROP_MYSQL_WRAPPER=1
fi

# Also drop the embedded-MySQL wrapper when its library is absent: no
# benchpress job runs the `mysql` bench, and `-lmysqld` is not shipped
# by public MySQL 8 packages (the headers alone are not enough to link).
# This covers public x86_64 builders; internal hosts that ship the
# library keep the wrapper.
if ! ls /usr/lib/mysql/libmysqld.* /usr/lib64/mysql/libmysqld.* >/dev/null 2>&1; then
  DROP_MYSQL_WRAPPER=1
fi
MAKE_EXTRA_ARGS=()
if [[ "${DROP_MYSQL_WRAPPER}" == "1" ]]; then
  MAKE_EXTRA_ARGS+=(MYSQL=0)
fi

# GCC >= 7 elevates -Wmaybe-uninitialized to a fatal error in masstree's
# str.hh; suppress just that warning when the compiler supports the flag.
MAYBE_UNINIT="$(echo | gcc -Wmaybe-uninitialized -E - >/dev/null 2>&1 \
                && echo '-Wno-error=maybe-uninitialized')"
CXX_CMD="g++ -std=gnu++0x ${MAYBE_UNINIT}"

# Silo's MODE=perf Makefile branch adds `-Werror -O2`, which on modern GCC
# (>= 11 on EL9) turns benign warnings (-Wformat-truncation in masstree's
# string_base.hh snprintf, -Wmaybe-uninitialized in btree.cc) into fatal
# errors. Because Silo's CXXFLAGS are appended after our CXX flags, a
# trailing `-Werror` re-enables errors for warnings we tried to demote.
# Strip the `-Werror` from the perf branch so our `-Wno-error=*` flags can
# take effect. Idempotent.
sed -i 's/-Werror -O2/-O2/g' Makefile

CXX="${CXX_CMD}" MODE=perf DEBUG=0 CHECK_INVARIANTS=0 USE_MALLOC_MODE=1 make dbtest "${MAKE_EXTRA_ARGS[@]}"

mkdir -p "${SILO_INSTALLATION_PREFIX}"
mv out-perf.masstree/benchmarks/dbtest "${SILO_BINARY_PATH}"

cd ../../

echo "Silo dbtest (commit ${SILO_GIT_COMMIT_TAG}) installed into ${SILO_BINARY_PATH}"
