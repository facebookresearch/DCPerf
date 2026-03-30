#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Install script for the CDN Benchmark (foss_revproxy).
#
# Builds proxygen and all dependencies from source, then builds the
# foss_revproxy binaries (traffic_client, proxy_server, content_server).
#
# Binaries are installed to benchmarks/cdn_bench/ relative to BENCHPRESS_ROOT.

set -Eeuo pipefail

CDN_PACKAGE_DIR="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${CDN_PACKAGE_DIR}/../../")"
INSTALL_DIR="${BENCHPRESS_ROOT}/benchmarks/cdn_bench"
BUILD_DIR="${CDN_PACKAGE_DIR}/_build"

# Pinned proxygen version for reproducible builds
PROXYGEN_GIT_REPO="https://github.com/facebook/proxygen.git"
PROXYGEN_GIT_TAG="v2026.01.05.00"

COLOR_GREEN="\033[0;32m"
COLOR_RED="\033[0;31m"
COLOR_OFF="\033[0m"

# Determine parallelism — cap at 16 to avoid OOM on high-core-count servers
NPROC=$(nproc)
JOBS=$((NPROC > 16 ? 16 : NPROC))

##############################################################################
# Detect environment: Meta internal (fbpkg) vs. external (git checkout)
##############################################################################
is_meta_internal() {
  [ -f "${BENCHPRESS_ROOT}/METADATA" ] || command -v fbpkg.fetch &>/dev/null
}

##############################################################################
# Ensure prerequisites (git, internet) are available
##############################################################################
ensure_prerequisites() {
  if ! command -v git &>/dev/null; then
    echo -e "${COLOR_GREEN}[ INFO ] Installing git${COLOR_OFF}"
    if command -v dnf &>/dev/null; then
      dnf install -y git
    elif command -v apt-get &>/dev/null; then
      apt-get update && apt-get install -yq git
    else
      echo -e "${COLOR_RED}[ ERROR ] Cannot install git — no supported package manager${COLOR_OFF}"
      exit 1
    fi
  fi

  # On Meta edge hosts, enable outbound internet via the standard forward proxy
  if is_meta_internal; then
    echo -e "TODO: Implement internal installation mechanism compatible with edge hosts(next diff)"
  fi
}

ensure_prerequisites

echo -e "${COLOR_GREEN}=== CDN Benchmark Install ===${COLOR_OFF}"
echo -e "${COLOR_GREEN}Package dir:    ${CDN_PACKAGE_DIR}${COLOR_OFF}"
echo -e "${COLOR_GREEN}Build dir:      ${BUILD_DIR}${COLOR_OFF}"
echo -e "${COLOR_GREEN}Install dir:    ${INSTALL_DIR}${COLOR_OFF}"
echo -e "${COLOR_GREEN}Parallelism:    ${JOBS} jobs${COLOR_OFF}"

# Check if already installed
if [ -x "${INSTALL_DIR}/traffic_client" ] && \
   [ -x "${INSTALL_DIR}/proxy_server" ] && \
   [ -x "${INSTALL_DIR}/content_server" ]; then
  echo -e "${COLOR_GREEN}CDN Benchmark binaries already installed, skipping build${COLOR_OFF}"
  echo -e "${COLOR_GREEN}To force reinstall, run: rm -rf ${INSTALL_DIR}${COLOR_OFF}"
  exit 0
fi

mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}"

##############################################################################
# Step 1: Clone proxygen at pinned version
##############################################################################
PROXYGEN_SRC="${BUILD_DIR}/proxygen"
if [ ! -d "${PROXYGEN_SRC}" ]; then
  echo -e "${COLOR_GREEN}[ INFO ] Cloning proxygen ${PROXYGEN_GIT_TAG}${COLOR_OFF}"
  git clone --depth 1 --branch "${PROXYGEN_GIT_TAG}" \
    "${PROXYGEN_GIT_REPO}" "${PROXYGEN_SRC}"
else
  echo -e "${COLOR_GREEN}[ INFO ] Proxygen source already present, skipping clone${COLOR_OFF}"
fi

##############################################################################
# Step 2: Build proxygen and all dependencies
##############################################################################
DEPS_DIR="${BUILD_DIR}/deps"
echo -e "${COLOR_GREEN}[ INFO ] Building proxygen and dependencies${COLOR_OFF}"
bash "${CDN_PACKAGE_DIR}/build_proxygen.sh" \
  -j "${JOBS}" \
  --proxygen-dir "${PROXYGEN_SRC}" \
  -p "${DEPS_DIR}"

##############################################################################
# Step 3: Build foss_revproxy
##############################################################################
FOSS_REVPROXY_SRC_DIR="${CDN_PACKAGE_DIR}/src"

if [ ! -f "${FOSS_REVPROXY_SRC_DIR}/CMakeLists.txt" ]; then
  echo -e "${COLOR_RED}[ ERROR ] CMakeLists.txt not found at: ${FOSS_REVPROXY_SRC_DIR}${COLOR_OFF}"
  exit 1
fi

if [ ! -d "${FOSS_REVPROXY_SRC_DIR}/ti/foss_revproxy" ]; then
  echo -e "${COLOR_RED}[ ERROR ] foss_revproxy source not found at: ${FOSS_REVPROXY_SRC_DIR}/ti/foss_revproxy${COLOR_OFF}"
  exit 1
fi

echo -e "${COLOR_GREEN}[ INFO ] Building foss_revproxy${COLOR_OFF}"
FOSS_BUILD_DIR="${BUILD_DIR}/foss_revproxy_build"
mkdir -p "${FOSS_BUILD_DIR}"
cd "${FOSS_BUILD_DIR}"

cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="${DEPS_DIR}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  "${FOSS_REVPROXY_SRC_DIR}"

make -j "${JOBS}"

##############################################################################
# Step 4: Install binaries
##############################################################################
echo -e "${COLOR_GREEN}[ INFO ] Installing binaries to ${INSTALL_DIR}${COLOR_OFF}"
cp -f "${FOSS_BUILD_DIR}/traffic_client" "${INSTALL_DIR}/"
cp -f "${FOSS_BUILD_DIR}/proxy_server" "${INSTALL_DIR}/"
cp -f "${FOSS_BUILD_DIR}/content_server" "${INSTALL_DIR}/"
chmod +x "${INSTALL_DIR}/traffic_client" "${INSTALL_DIR}/proxy_server" "${INSTALL_DIR}/content_server"

echo -e "${COLOR_GREEN}=== CDN Benchmark Install Complete ===${COLOR_OFF}"
echo -e "${COLOR_GREEN}Binaries installed:${COLOR_OFF}"
ls -la "${INSTALL_DIR}/"
