#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Run FeedSim
################################################################################

run_feedsim () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run FeedSim"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[RUN] Running FeedSim ..."
  (print_exec conda run --no-capture-output ${env_prefix} \
  python ./benchpress_cli.py run feedsim_autoscale -i "'{\"extra_args\": \"-q 5 -d 20\"}'") || true
  cat benchpress.log

  # Print all feedsim results and logs from benchmark_metrics_* directories
  echo ""
  echo "[INFO] Searching for FeedSim results in benchmark_metrics_* directories..."
  for metrics_dir in benchmark_metrics_*/; do
    if [ -d "$metrics_dir" ]; then
      echo ""
      echo "=== Found directory: $metrics_dir ==="

      # Print all feedsim_results*.txt files
      for results_file in "${metrics_dir}"feedsim_results*.txt; do
        if [ -f "$results_file" ]; then
          echo ""
          echo "--- Contents of $results_file ---"
          cat "$results_file"
        fi
      done

      # Print all feedsim-multi-inst-*.log files
      for log_file in "${metrics_dir}"feedsim-multi-inst-*.log; do
        if [ -f "$log_file" ]; then
          echo ""
          echo "--- Contents of $log_file ---"
          cat "$log_file"
        fi
      done
    fi
  done

  # Run benchpress and capture output
  # if output=$(print_exec conda run --no-capture-output ${env_prefix} python ./benchpress_cli.py run feedsim_autoscale -i "'{\"extra_args\": \"-q 5 -d 20\"}'" 2>&1); then
  #   echo "$output"
  # else
  #   echo "ERROR: Benchpress command failed to execute"
  #   return 1
  # fi

  # # Extract final_achieved_qps from the output and validate
  # final_qps=$(echo "$output" | grep -A3 '"overall"' | grep -o '"final_achieved_qps":[^,]*' | grep -o '[0-9.]*' | head -1)

  # if [ -z "$final_qps" ]; then
  #     echo "ERROR: Could not extract final_achieved_qps from output"
  #     return 1
  # fi

  # # Check if final_achieved_qps is greater than zero
  # if (( $(echo "$final_qps > 0" | bc -l) )); then
  #     echo "SUCCESS: final_achieved_qps ($final_qps) is greater than zero"
  #     return 0
  # else
  #     echo "FAILURE: final_achieved_qps ($final_qps) is not greater than zero"
  #     return 1
  # fi
}
