#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# shellcheck disable=SC2034
ARCH="$(uname -m)"
WDL_SOURCE="${WDL_ROOT}/wdl_sources"
WDL_BUILD="${WDL_ROOT}/wdl_build"
WDL_DATASETS="${WDL_ROOT}/datasets"
MKL_VERSION="2025.3.0.462"
ARMPL_VERSION="26.01"
# Determine OS version
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"

BREPS_LFILE=/tmp/wdl_log.txt

benchreps_tell_state() {
    date +"%Y-%m-%d_%T ${1}" >> "$BREPS_LFILE"
}

has_real_conda() {
    # 1. Check if 'conda' command exists at all
    if ! command -v conda >/dev/null 2>&1; then
        return 1 # False: Not found
    fi

    # 2. Check if conda base exists. On dev servers, conda might be
    #    pointing to /usr/bin/local/conda that is a wrapper just telling
    #    the user how to install conda. If that's the case, fail and tell
    #.   the user to install conda.
    local conda_base
    conda_base=$(conda info --base 2>/dev/null)
    if [ -z "$conda_base" ]; then
        echo "You might be running this on a dev server. Please do the following and restart this command."
        echo "  1. Install conda on your dev server (feature install conda)."
        echo "  2. sudo conda init bash"
        echo "  3. exec bash"
        exit 1
    fi

    return 0
}

in_conda_env() {
    # The :- suffix prevents the "unbound variable" error
    [[ -n "${CONDA_PREFIX:-}" && -d "${CONDA_PREFIX:-}" ]]
}

source_conda() {
    local conda_sh=""
    local conda_base
    conda_base=$(conda info --base 2>/dev/null)
    if [ -f "$conda_base/etc/profile.d/conda.sh" ]; then
        # 1. Check the default Miniconda first
        conda_sh="$conda_base/etc/profile.d/conda.sh"
        echo "Sourced local conda from $conda_base"

    elif [ -f "/etc/profile.d/conda.sh" ]; then
        # 2. Check system-wide location if the default location fails
        conda_sh=/etc/profile.d/conda.sh
        echo "Sourced system conda from /etc/profile.d/conda.sh"

    elif [ -f "${WDL_ROOT}"/miniconda3/etc/profile.d/conda.sh ]; then
        # 3. Check the local Miniconda if the system-wide location fails
        conda_sh="${WDL_ROOT}/miniconda3/etc/profile.d/conda.sh"
        echo "Sourced local conda from ${WDL_ROOT}/miniconda3/etc/profile.d/conda.sh"

    else
        echo "Error: conda.sh not found in local or system paths."
        exit 1
    fi

    # shellcheck disable=SC1090
    source "$conda_sh"
}

get_git_proxy_args() {
    : # Replace with your proxy args
}

get_curl_proxy_args() {
    : # Replace with your proxy args
}

get_conda_proxy_args() {
    : # Replace with your proxy args
}

get_march_for_host() {
    if [ "$ARCH" = "aarch64" ]; then
        # Compare GCC macro sets (sorted to avoid ordering differences)
        local base native
        base="$(echo | gcc -dM -E - | grep -E 'ARM_FEATURE' | sort)"
        native="$(echo | gcc -dM -E -march=native - | grep -E 'ARM_FEATURE' | sort)"

        if [[ "$base" != "$native" ]]; then
            # -march=native actually unlocks more features, so use it.
            echo "-march=native"
            return 0
        fi

        # Otherwise, build -march from lscpu Flags
        local flags march_base ext=""
        flags="$(lscpu | awk -F': *' 'tolower($1)=="flags"{print $2; exit}')"

        # Choose a conservative base ISA (Neoverse N1).
        march_base="armv8.2-a"

        # crypto: GCC uses +crypto to cover aes/sha1/sha2/pmull
        if grep -qwE 'aes|sha1|sha2|pmull' <<<"$flags"; then
            ext+="+crypto"
        fi

        grep -qw rng      <<<"$flags" && ext+="+rng"
        grep -qw asimddp  <<<"$flags" && ext+="+dotprod"
        grep -qw lrcpc    <<<"$flags" && ext+="+rcpc"
        grep -qw sha3     <<<"$flags" && ext+="+sha3"
        grep -qw i8mm     <<<"$flags" && ext+="+i8mm"

        # bf16 and fp16
        grep -qw fphp    <<<"$flags" && ext+="+fp16"
        grep -qw asimdhp <<<"$flags" && ext+="+fp16fml"
        grep -qw bf16    <<<"$flags" && ext+="+bf16"

        # SVE family
        grep -qw sve      <<<"$flags" && ext+="+sve"
        grep -qw sve2     <<<"$flags" && ext+="+sve2"

        echo "-march=${march_base}${ext}"
    fi
}
