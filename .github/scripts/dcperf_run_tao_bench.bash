#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# tao_bench Run Functions
################################################################################

run_tao_bench () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run Tao_bench"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi


  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[RUN] Running Tao_bench ..."

  # Run benchpress and capture output
  if output=$(print_exec conda run --no-capture-output ${env_prefix} python ./benchpress_cli.py run tao_bench_standalone -i "'{\"memsize\": 0.5, \"stats_interval\":1000, \"warmup_time\": 60, \"test_time\": 60}'" 2>&1); then
    echo "$output"
  else
    echo "ERROR: Benchpress command failed to execute"
    return 1
  fi

  # Extract total_qps from the output and validate
    total_qps=$(echo "$output" | grep -o '"total_qps":[^,]*' | grep -o '[0-9.]*' | head -1)

  if [ -z "$total_qps" ]; then
      echo "ERROR: Could not extract total_qps from output"
      return 1
  fi

  # Check if total_qps is greater than zero
  if (( $(echo "$total_qps > 0" | bc -l) )); then
      echo "SUCCESS: total_qps ($total_qps) is greater than zero"
      return 0
  else
      echo "FAILURE: total_qps ($total_qps) is not greater than zero"
      return 1
  fi
}
