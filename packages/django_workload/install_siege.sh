#!/bin/bash
set -e
set -x

SIEGE_GIT_REPO='https://github.com/JoeDog/siege.git'
SIEGE_GIT_RELEASE_TAG='v4.0.7'

DJANGO_PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)
BENCHPRESS_ROOT="$(readlink -f "${DJANGO_PKG_ROOT}/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
mkdir -p "$BENCHMARKS_DIR"

LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

if [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y autoconf automake zlib-devel
elif [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y autoconf automake zlib1g-dev 
fi

SIEGE_INSTALLATION_PREFIX="${BENCHMARKS_DIR}/siege"
SIEGE_BINARY_PATH="${SIEGE_INSTALLATION_PREFIX}/bin/siege"
if [[ -f "$SIEGE_BINARY_PATH"  && -x "$SIEGE_BINARY_PATH" ]]; then
  echo "siege is already installed into ${SIEGE_INSTALLATION_PREFIX}"
  exit 0
fi

rm -rf build
mkdir -p build
cd build/

# shellcheck disable=SC2046
git clone "$SIEGE_GIT_REPO"
cd siege/
git checkout "$SIEGE_GIT_RELEASE_TAG"
./utils/bootstrap
./configure
make -j4
make install
cd ../../

rm -rf build/

# copy siege config file
mkdir -p "${HOME}/.siege"
if [ -f "${HOME}/.siege/siege.conf" ]; then
  mv -f "${HOME}/.siege/siege.conf" "${HOME}/.siege/siege.conf.bak"
fi
cp "${DJANGO_PKG_ROOT}/templates/siege.conf" "${HOME}/.siege/siege.conf"

echo "siege installed into ${SIEGE_INSTALLATION_PREFIX}"
