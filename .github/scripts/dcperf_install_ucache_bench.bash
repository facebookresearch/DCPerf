#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Install UCacheBench
################################################################################

install_ucache_bench () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Install UCacheBench"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[INSTALL] Installing UCacheBench ..."
  # shellcheck disable=SC2086 # ${env_prefix} word-splits intentionally ("-n <name>" / "-p <path>")
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py install ucache_bench_memory) || return 1
}


################################################################################
# Verify UCacheBench Install
#
# The installer already runs the OSS smoke test (ctest
# ucachebench_oss_smoke) as its last build step. This re-checks the
# installed artifacts and re-runs that smoke test so the workflow has
# an explicit verification step, and checks the prepared Carbon
# sources the OSS build compiled: the internal metadata blocks must
# have been stripped and no folly likely/unlikely macros may remain.
################################################################################

verify_ucache_bench_install () {
  echo "################################################################################"
  echo "# Verify UCacheBench Install"
  echo "#"
  echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
  echo "################################################################################"
  echo ""

  local build_dir="packages/ucache_bench/build"
  local prepared_header="${build_dir}/protocol/carbon-gen/UcacheBenchThriftTransport.h"

  echo "[VERIFY] Checking installed binaries ..."
  local binary
  for binary in \
    benchmarks/ucache_bench/server/ucachebench_server \
    benchmarks/ucache_bench/client/ucachebench_client \
    benchmarks/ucache_bench/client/bind_source.so; do
    if [ ! -x "$binary" ]; then
      echo "ERROR: installed binary is missing or not executable: $binary" >&2
      return 1
    fi
    echo "[VERIFY] Found executable: $binary"
  done

  echo "[VERIFY] Checking prepared Carbon sources ..."
  if [ ! -s "$prepared_header" ]; then
    echo "ERROR: prepared header is missing or empty: $prepared_header" >&2
    return 1
  fi
  local symbol
  for symbol in getPrivacyLibAgenticContext getKcbIdentity; do
    if grep -q "$symbol" "$prepared_header"; then
      echo "ERROR: prepared header still references internal metadata: $symbol" >&2
      return 1
    fi
  done
  if grep -Eq 'FOLLY_(LIKELY|UNLIKELY)' "$prepared_header"; then
    echo "ERROR: prepared header still uses folly likely/unlikely macros" >&2
    return 1
  fi
  if ! grep -Fq '[[unlikely]]' "$prepared_header"; then
    echo "ERROR: prepared header has no [[unlikely]] annotations" >&2
    return 1
  fi

  echo "[VERIFY] Re-running OSS smoke test ..."
  (print_exec ctest --test-dir "$build_dir" \
    -R '^ucachebench_oss_smoke$' --output-on-failure) || return 1

  echo "[VERIFY] UCacheBench install verified"
}
