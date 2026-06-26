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
# `install_markers` in benchmarks_internal.yml can detect a completed
# install.

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"

# System deps. fb-fwdproxy-config provides the fwdproxy-config helper used
# below to give git access to public github through fwdproxy. autoconf /
# automake / libtool are required because masstree's build runs
# `autoreconf -i`. zlib-devel is required for dbtest's `-lz` link step.
dnf install -y \
    fb-fwdproxy-config \
    autoconf \
    automake \
    libtool \
    jemalloc-devel \
    numactl-devel \
    libdb-cxx-devel \
    mysql-devel \
    libaio-devel \
    openssl-devel \
    zlib-devel

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

# shellcheck disable=SC2046
git $(fwdproxy-config --git-command git) clone "$SILO_GIT_REPO_URL"
cd silo
git checkout -b benchpress "${SILO_GIT_COMMIT_TAG}"

# The masstree submodule's .gitmodules pins git:// URLs that github no
# longer serves. Rewrite to https:// before recursing.
sed -i 's|git://|https://|g' .gitmodules
# shellcheck disable=SC2046
git $(fwdproxy-config --git-command git) submodule update --init --recursive

# Apply the cgroup-cpuset-awareness + no-NUMA patch (authored against the pinned
# commit above). It (a) removes Silo's explicit NUMA memory hint so DB-pool
# placement is pure first-touch, and (b) adds a `dbtest --cgroup-aware` runtime
# flag (default off) that pins each worker to its inherited cgroup cpuset CPU.
# Default behavior (no flag) matches stock Silo. Applied to the freshly-cloned
# tree, so a plain check-then-apply is sufficient.
SILO_PATCH="${SCRIPT_DIR}/patches/silo-cgroup-aware-no-numa.patch"
git apply --check "${SILO_PATCH}" && git apply "${SILO_PATCH}"

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

CXX="${CXX_CMD}" MODE=perf DEBUG=0 CHECK_INVARIANTS=0 USE_MALLOC_MODE=1 make dbtest

mkdir -p "${SILO_INSTALLATION_PREFIX}"
mv out-perf.masstree/benchmarks/dbtest "${SILO_BINARY_PATH}"

cd ../../

echo "Silo dbtest (commit ${SILO_GIT_COMMIT_TAG}) installed into ${SILO_BINARY_PATH}"
