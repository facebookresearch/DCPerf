#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Install FeedSim
################################################################################

install_feedsim () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Install FeedSim"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[INSTALL] Installing FeedSim ..."
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py install feedsim_dlrm) || return 1
}


################################################################################
# Run FeedSim smoke test (small fixed-QPS run for resource-limited CI containers)
################################################################################

run_feedsim_smoke_test () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run FeedSim Smoke Test"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Fixed-QPS overrides for the smoke run, kept tiny for CI containers.
  local smoke_input='{"fixed_qps": "10", "fixed_qps_duration": "60"}'

  echo "[SMOKE] Running FeedSim fixed-QPS smoke test ..."
  # NOTE: print_exec evals its arguments, so the JSON must reach eval
  # single-quoted to survive as one argv element.
  # shellcheck disable=SC2086 # ${env_prefix} word-splits intentionally ("-n <name>" / "-p <path>")
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py run feedsim_dlrm_mini -i "'${smoke_input}'" --ignore-sys-specs-errors) || return 1
}
