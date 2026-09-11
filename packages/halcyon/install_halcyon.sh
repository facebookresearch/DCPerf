#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../..")"
INSTALL_ROOT="${BENCHPRESS_ROOT}/benchmarks/halcyon"
SOURCE_ROOT="${INSTALL_ROOT}/src"
DEPS_ROOT="${INSTALL_ROOT}/deps"
BUILD_ROOT="${INSTALL_ROOT}/build"
PYTHON_SYSTEM="/usr/bin/python3"
FBTHRIFT_TAG="v2026.09.07.00"
FOLLY_TAG="v2026.09.07.00"
LIBEVENT_TAG="release-2.1.12-stable"
LIBURING_TAG="liburing-2.15"
BINUTILS_VERSION="2.42"
BINUTILS_SHA256="f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"
BINUTILS_URL="https://mirrors.ocf.berkeley.edu/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
BUILD_JOBS="${HALCYON_BUILD_JOBS:-$(nproc)}"
if ((BUILD_JOBS > 32)); then
  BUILD_JOBS=32
fi

retry_network_command() {
  local attempt=1
  until "$@"; do
    if ((attempt >= 3)); then
      return 1
    fi
    echo "Network command failed; retrying (attempt $((attempt + 1))/3)" >&2
    sleep "$((attempt * 2))"
    attempt=$((attempt + 1))
  done
}

if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
fi

case "${ID:-}" in
  ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y autoconf automake bison build-essential cmake flex \
      git libboost-all-dev libdouble-conversion-dev libevent-dev libgflags-dev \
      libaio-dev liblz4-dev liblzma-dev libsnappy-dev libssl-dev \
      libre2-dev librocksdb-dev libsodium-dev libtool liburing-dev libzstd-dev \
      ninja-build patchelf pkg-config python3-dev python3-pip python3-venv \
      zlib1g-dev
    ;;
  centos|rhel|rocky)
    dnf install -y autoconf automake binutils-devel bison boost-devel \
      bzip2-devel cmake \
      double-conversion-devel fmt-devel gcc gcc-c++ git gflags-devel \
      glog-devel libaio-devel libevent-devel libsodium-devel libtool \
      libunwind-devel liburing-devel libzstd-devel lz4-devel make ninja-build \
      openssl openssl-devel openssl-libs patchelf pkgconf-pkg-config \
      python3-devel python3-pip \
      re2-devel rocksdb-devel snappy-devel xxhash-devel xz-devel zlib-devel
    if [[ "${VERSION_ID:-}" == 9* && "${IS_INTERNAL_TEST:-0}" != "1" ]]; then
      dnf install -y gcc-toolset-13-gcc gcc-toolset-13-gcc-c++
      # shellcheck disable=SC1091
      source /opt/rh/gcc-toolset-13/enable
      export CC=/opt/rh/gcc-toolset-13/root/usr/bin/gcc
      export CXX=/opt/rh/gcc-toolset-13/root/usr/bin/g++
    fi
    ;;
  *)
    echo "Unsupported distribution: ${ID:-unknown}" >&2
    exit 2
    ;;
esac

mkdir -p "${SOURCE_ROOT}" "${DEPS_ROOT}/installed" "${BUILD_ROOT}"
if [[ ! -d "${SOURCE_ROOT}/liburing/.git" ]]; then
  retry_network_command git clone --branch "${LIBURING_TAG}" --depth 1 \
    https://github.com/axboe/liburing.git "${SOURCE_ROOT}/liburing"
fi
if ! git -C "${SOURCE_ROOT}/liburing" rev-parse --verify --quiet \
  "refs/tags/${LIBURING_TAG}" >/dev/null; then
  retry_network_command git -C "${SOURCE_ROOT}/liburing" fetch --depth 1 origin \
    "refs/tags/${LIBURING_TAG}:refs/tags/${LIBURING_TAG}"
fi
git -C "${SOURCE_ROOT}/liburing" checkout --detach "${LIBURING_TAG}"
LIBURING_INSTALL="${DEPS_ROOT}/installed/liburing"
(
  cd "${SOURCE_ROOT}/liburing"
  ./configure --prefix="${LIBURING_INSTALL}"
  make -C src --jobs="${BUILD_JOBS}"
  make install
)

export CMAKE_PREFIX_PATH="${LIBURING_INSTALL}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
export PKG_CONFIG_PATH="${LIBURING_INSTALL}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export LD_LIBRARY_PATH="${LIBURING_INSTALL}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

if [[ ! -d "${SOURCE_ROOT}/libevent/.git" ]]; then
  retry_network_command git clone --branch "${LIBEVENT_TAG}" --depth 1 \
    https://github.com/libevent/libevent.git "${SOURCE_ROOT}/libevent"
fi
if ! git -C "${SOURCE_ROOT}/libevent" rev-parse --verify --quiet \
  "refs/tags/${LIBEVENT_TAG}" >/dev/null; then
  retry_network_command git -C "${SOURCE_ROOT}/libevent" fetch --depth 1 origin \
    "refs/tags/${LIBEVENT_TAG}:refs/tags/${LIBEVENT_TAG}"
fi
git -C "${SOURCE_ROOT}/libevent" checkout --detach "${LIBEVENT_TAG}"
LIBEVENT_INSTALL="${DEPS_ROOT}/installed/libevent"
cmake -S "${SOURCE_ROOT}/libevent" -B "${BUILD_ROOT}/libevent" -G Ninja \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${LIBEVENT_INSTALL}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DEVENT__DISABLE_BENCHMARK=ON \
  -DEVENT__DISABLE_REGRESS=ON \
  -DEVENT__DISABLE_SAMPLES=ON \
  -DEVENT__DISABLE_TESTS=ON
cmake --build "${BUILD_ROOT}/libevent" --parallel "${BUILD_JOBS}"
cmake --install "${BUILD_ROOT}/libevent"

if [[ ! -d "${SOURCE_ROOT}/fbthrift/.git" ]]; then
  retry_network_command git clone --branch "${FBTHRIFT_TAG}" --depth 1 \
    https://github.com/facebook/fbthrift.git "${SOURCE_ROOT}/fbthrift"
fi
if ! git -C "${SOURCE_ROOT}/fbthrift" rev-parse --verify --quiet \
  "refs/tags/${FBTHRIFT_TAG}" >/dev/null; then
  retry_network_command git -C "${SOURCE_ROOT}/fbthrift" fetch --depth 1 origin \
    "refs/tags/${FBTHRIFT_TAG}:refs/tags/${FBTHRIFT_TAG}"
fi
git -C "${SOURCE_ROOT}/fbthrift" checkout --detach "${FBTHRIFT_TAG}"

# getdeps only places direct dependencies on the build command's runtime
# library path. thrift1 links glog through shared Folly and runs during this
# build, so glog must also be a direct FBThrift dependency.
FBTHRIFT_MANIFEST="${SOURCE_ROOT}/fbthrift/build/fbcode_builder/manifests/fbthrift"
if ! sed -n '/^\[dependencies\]$/,/^\[/p' "${FBTHRIFT_MANIFEST}" | grep -qx glog; then
  sed -i '/^\[dependencies\]$/a glog' "${FBTHRIFT_MANIFEST}"
fi

if [[ ! -d "${SOURCE_ROOT}/folly/.git" ]]; then
  retry_network_command git clone --branch "${FOLLY_TAG}" --depth 1 \
    https://github.com/facebook/folly.git "${SOURCE_ROOT}/folly"
fi
if ! git -C "${SOURCE_ROOT}/folly" rev-parse --verify --quiet \
  "refs/tags/${FOLLY_TAG}" >/dev/null; then
  retry_network_command git -C "${SOURCE_ROOT}/folly" fetch --depth 1 origin \
    "refs/tags/${FOLLY_TAG}:refs/tags/${FOLLY_TAG}"
fi
git -C "${SOURCE_ROOT}/folly" checkout --detach "${FOLLY_TAG}"

# Folly's modular CMake build omits the AArch64 resolver sources that export
# __folly_memcpy and __folly_memset. Add them to both optimized targets.
if [[ "$(uname -m)" == "aarch64" ]]; then
  FOLLY_AOR_CMAKE="${SOURCE_ROOT}/folly/folly/external/aor/CMakeLists.txt"
  for selector in memcpy memset; do
    if ! grep -Fq "../../${selector}_select_aarch64.cpp" \
      "${FOLLY_AOR_CMAKE}"; then
      sed -i "/^    ${selector}-advsimd\\.S$/i\\    ../../${selector}_select_aarch64.cpp" \
        "${FOLLY_AOR_CMAKE}"
    fi
  done
fi

# GNU's redirector can drop archived releases. Use the stable mirror/version
# already exercised by DCPerf's other Folly-based installers.
GETDEPS_MANIFESTS="${SOURCE_ROOT}/fbthrift/build/fbcode_builder/manifests"
for manifest_name in libiberty libiberty-python; do
  manifest="${GETDEPS_MANIFESTS}/${manifest_name}"
  if [[ ! -f "${manifest}" ]]; then
    continue
  fi
  sed -i \
    -e "s|^url = .*binutils-.*\.tar\.xz$|url = ${BINUTILS_URL}|" \
    -e "s|^sha256 = .*$|sha256 = ${BINUTILS_SHA256}|" \
    -e "s|^subdir = binutils-.*/libiberty$|subdir = binutils-${BINUTILS_VERSION}/libiberty|" \
    "${manifest}"
done

DEPS_BUILD_MARKER="${DEPS_ROOT}/installed/.halcyon-deps-${FBTHRIFT_TAG}-pinned-shared"
if [[ ! -f "${DEPS_BUILD_MARKER}" ]]; then
  "${PYTHON_SYSTEM}" "${SOURCE_ROOT}/fbthrift/build/fbcode_builder/getdeps.py" \
    --allow-system-packages build fbthrift \
    --src-dir "${SOURCE_ROOT}/fbthrift" \
    --src-dir "folly:${SOURCE_ROOT}/folly" \
    --extra-cmake-defines \
    '{"BUILD_SHARED_LIBS":"ON","CMAKE_POSITION_INDEPENDENT_CODE":"ON"}' \
    --num-jobs "${BUILD_JOBS}" \
    --no-tests \
    --only-deps \
    --scratch-path "${DEPS_ROOT}"
  touch "${DEPS_BUILD_MARKER}"
fi

PYTHON_SYSTEM_VERSION="$("${PYTHON_SYSTEM}" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
PYTHON_BUILD_ENV="${DEPS_ROOT}/python-${PYTHON_SYSTEM_VERSION}"
"${PYTHON_SYSTEM}" -m venv "${PYTHON_BUILD_ENV}"
"${PYTHON_BUILD_ENV}/bin/pip" install --disable-pip-version-check \
  auditwheel==6.4.2 Cython==3.1.3 setuptools==80.9.0 wheel==0.45.1
PYTHON_BUILD="${PYTHON_BUILD_ENV}/bin/python3"
PYTHON_BUILD_VERSION="${PYTHON_SYSTEM_VERSION}"
export PATH="${PYTHON_BUILD_ENV}/bin:${PATH}"

DEPS_INSTALL="${DEPS_ROOT}/installed/fbthrift"
DEPENDENCY_PREFIXES=("${LIBURING_INSTALL}" "${LIBEVENT_INSTALL}")
for dependency_prefix in "${DEPS_ROOT}"/installed/*; do
  if [[ -d "${dependency_prefix}" && "${dependency_prefix}" != "${LIBURING_INSTALL}" ]]; then
    DEPENDENCY_PREFIXES+=("${dependency_prefix}")
  fi
done
DEPS_ENV_PREFIX="$(IFS=:; echo "${DEPENDENCY_PREFIXES[*]}")"
export CMAKE_PREFIX_PATH="${DEPS_ENV_PREFIX}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
DEPENDENCY_LIBRARY_PATHS=()
for dependency_prefix in "${DEPENDENCY_PREFIXES[@]}"; do
  for library_directory in lib lib64; do
    if [[ -d "${dependency_prefix}/${library_directory}" ]]; then
      DEPENDENCY_LIBRARY_PATHS+=("${dependency_prefix}/${library_directory}")
    fi
  done
done
if ((${#DEPENDENCY_LIBRARY_PATHS[@]} > 0)); then
  DEPS_LIBRARY_PATH="$(IFS=:; echo "${DEPENDENCY_LIBRARY_PATHS[*]}")"
  export LD_LIBRARY_PATH="${DEPS_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

"${PYTHON_SYSTEM}" "${SOURCE_ROOT}/fbthrift/build/fbcode_builder/getdeps.py" \
  --allow-system-packages build folly \
  --src-dir "${SOURCE_ROOT}/folly" \
  --build-dir "${DEPS_ROOT}/build/folly-python-${PYTHON_BUILD_VERSION}" \
  --extra-cmake-defines \
  "{\"BUILD_SHARED_LIBS\":\"ON\",\"CMAKE_POSITION_INDEPENDENT_CODE\":\"ON\",\"PYTHON_EXTENSIONS\":\"ON\",\"Python3_EXECUTABLE\":\"${PYTHON_BUILD}\"}" \
  --install-dir "${DEPS_ROOT}/installed/folly" \
  --num-jobs "${BUILD_JOBS}" \
  --no-deps \
  --no-tests \
  --scratch-path "${DEPS_ROOT}"

"${PYTHON_SYSTEM}" "${SOURCE_ROOT}/fbthrift/build/fbcode_builder/getdeps.py" \
  --allow-system-packages build fbthrift \
  --src-dir "${SOURCE_ROOT}/fbthrift" \
  --build-dir "${DEPS_ROOT}/build/fbthrift-python-${PYTHON_BUILD_VERSION}" \
  --extra-cmake-defines \
  "{\"BUILD_SHARED_LIBS\":\"ON\",\"CMAKE_POSITION_INDEPENDENT_CODE\":\"ON\",\"Python3_EXECUTABLE\":\"${PYTHON_BUILD}\",\"PYTHON_EXECUTABLE\":\"${PYTHON_BUILD}\",\"thrift_python\":\"ON\"}" \
  --num-jobs "${BUILD_JOBS}" \
  --no-deps \
  --no-tests \
  --install-dir "${DEPS_INSTALL}" \
  --scratch-path "${DEPS_ROOT}"

DEPENDENCY_PREFIXES=("${DEPS_INSTALL}" "${DEPENDENCY_PREFIXES[@]}")
DEPS_CMAKE_PREFIX="$(IFS=';'; echo "${DEPENDENCY_PREFIXES[*]}")"
if [[ -d "${DEPS_INSTALL}/lib" ]]; then
  export LD_LIBRARY_PATH="${DEPS_INSTALL}/lib:${LD_LIBRARY_PATH}"
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_ROOT}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHALCYON_BUILD_TESTS=ON \
  -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
  -DCMAKE_PREFIX_PATH="${DEPS_CMAKE_PREFIX}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_ROOT}"
cmake --build "${BUILD_ROOT}" --parallel "${BUILD_JOBS}"
ctest --test-dir "${BUILD_ROOT}" --output-on-failure
cmake --install "${BUILD_ROOT}"

"${PYTHON_SYSTEM}" -m venv --clear "${INSTALL_ROOT}/venv"
THRIFT_WHEELS=("${DEPS_INSTALL}"/share/thrift/wheels/*.whl)
if [[ ${#THRIFT_WHEELS[@]} -ne 1 || ! -f "${THRIFT_WHEELS[0]}" ]]; then
  echo "Expected exactly one FBThrift Python wheel in ${DEPS_INSTALL}/share/thrift/wheels" >&2
  exit 1
fi
"${INSTALL_ROOT}/venv/bin/pip" install --disable-pip-version-check \
  click==8.1.8 "${THRIFT_WHEELS[0]}"

echo "Halcyon installed in ${INSTALL_ROOT}"
