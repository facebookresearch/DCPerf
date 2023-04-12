#!/bin/bash
set -Eeuo pipefail

# FIXME(cltorres): Copy/link bpkgs benchmark contents into the BP_TMP automatically.
BPKGS_TAO_BENCH_ROOT="$(dirname "$(readlink -f "$0")")" # Path to dir with this file.
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_TAO_BENCH_ROOT/../..")"
COMMON_DIR="${BENCHPRESS_ROOT}/packages/common"

# FIXME(cltorres): Remove once we make the BP_TMP the default working diretory
cd "$BP_TMP" || exit 1

# Path to folly's source code repository
FOLLY_REPO_PATH=/root/folly

if [ -z "$FOLLY_REPO_PATH" ]; then
    echo "Please set FOLLY_REPO_PATH first!";
    exit 1;
fi

# === Install system dependencies ===
SYS_PKGS=(
    automake
    openssl-devel
    lz4
    libevent-devel
    glog-devel
    boost
    pcre-devel
)
for pkg in "${SYS_PKGS[@]}"; do
    sudo dnf install -y "$pkg";
done

# === Build and install memcached (tao_bench_server) ===
# Download original memcached source
curl http://www.memcached.org/files/memcached-1.6.5.tar.gz > memcached-1.6.5.tar.gz

# Extract
tar -zxf memcached-1.6.5.tar.gz
cd memcached-1.6.5 || exit 1

# We'll need to run autogen.sh if config.h.in does not exist in memcached's source
# In this case, we will require autotools and automake
if ! [ -f "config.h.in" ]; then
    ./autogen.sh
fi
# Patch w/ Tao Bench changes
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/tao_bench_memcached_0001.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0002-tao_bench_memcached_oom_handling.diff"
patch -p1 -i "${BPKGS_TAO_BENCH_ROOT}/0003-tao_bench_thread_pool_naming.diff"

# Find the path to folly and fmt
cd "$FOLLY_REPO_PATH"
FOLLY_INSTALLED_PATH="$("${FOLLY_REPO_PATH}/build/fbcode_builder/getdeps.py" --allow-system-packages show-inst-dir folly)"
FMT_INSTALLED_PATH="$("${FOLLY_REPO_PATH}/build/fbcode_builder/getdeps.py" --allow-system-packages show-inst-dir fmt)"
cd -

if ! [ -d "${FOLLY_INSTALLED_PATH}/lib64" ]; then
    ln -s -f "${FOLLY_INSTALLED_PATH}/lib" "${FOLLY_INSTALLED_PATH}/lib64"
fi

# Build and install
./configure --with-folly="${FOLLY_INSTALLED_PATH}" --with-fmt="${FMT_INSTALLED_PATH}" \
            --disable-coverage --enable-tls
make
mkdir -p "${BENCHPRESS_ROOT}/benchmarks/tao_bench"
cp memcached "${BENCHPRESS_ROOT}/benchmarks/tao_bench/tao_bench_server"
cp "${BPKGS_TAO_BENCH_ROOT}/db_items.json" "${BENCHPRESS_ROOT}/benchmarks/tao_bench/"
cp "${BPKGS_TAO_BENCH_ROOT}/leader_sizes.json" "${BENCHPRESS_ROOT}/benchmarks/tao_bench/"
cp -r "${COMMON_DIR}/affinitize" "${BENCHPRESS_ROOT}/benchmarks/tao_bench/"

cd "$BP_TMP" || exit 1

# === Build and install memtier_client (tao_bench_client) ===
# Copy memtier_client source
cp -r "${COMMON_DIR}/memtier_client" "$BP_TMP/"
cd memtier_client || exit 1

# Build and install
autoreconf --force --install
./configure --enable-tls
make || ( automake --add-missing && make )
cp memtier_benchmark "${BENCHPRESS_ROOT}/benchmarks/tao_bench/tao_bench_client"

# Extract certificates
tar -zxf "${COMMON_DIR}/certs.tar.gz" -C "${BENCHPRESS_ROOT}/benchmarks/tao_bench"

cd "$BP_TMP" || exit 1
