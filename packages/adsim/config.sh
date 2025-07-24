#!/bin/bash

export ADSIM_C_COMPILER=/usr/bin/clang
export ADSIM_CXX_COMPILER=/usr/bin/clang++
#export ADSIM_C_COMPILER=/mnt/gvfs/third-party2/llvm-fb/08eda565c37606ef147c9e40eeb5e97ee19df41e/12/platform010/72a2ff8/bin/clang
#export ADSIM_CXX_COMPILER=/mnt/gvfs/third-party2/llvm-fb/08eda565c37606ef147c9e40eeb5e97ee19df41e/12/platform010/72a2ff8/bin/clang++

# Root directory of the Benchpress project
# shellcheck disable=SC2155
export ADSIM_PROJ_ROOT="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
export ADSIM_DEPS_DIR="${ADSIM_PROJ_ROOT}/deps"
export ADSIM_MAIN_SRC_DIR="${ADSIM_PROJ_ROOT}/adsim"
export ADSIM_STAGING_DIR="${ADSIM_PROJ_ROOT}/staging"
# The reason of having a separate staging dir for FBGEMM
# is that it uses an older version of glog and googletest
export FBGEMM_STAGING_DIR="${ADSIM_PROJ_ROOT}/fbgemm-build"

function quiet_pushd() {
    command pushd "$@" > /dev/null || exit 1
}

function quiet_popd() {
    command popd > /dev/null || exit 1
}
