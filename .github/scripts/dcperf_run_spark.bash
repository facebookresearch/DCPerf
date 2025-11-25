#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Run Spark
################################################################################

run_spark () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run Spark"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[RUN] Running Spark ..."
  wget https://github.com/facebookresearch/DCPerf-datasets/releases/download/v1.0/bpc_t93586_s2_synthetic_1GB.tar.gz
  tar -xzf bpc_t93586_s2_synthetic_1GB.tar.gz
  mkdir /flash23
  cp -rf bpc_t93586_s2_synthetic_1GB /flash23/
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py run spark_standalone_remote_mini -i "'{\"dataset_name\":\"bpc_t93586_s2_synthetic_1GB\"}'") || return 1

  # Print all files from benchmark_metrics_*/work directories
  echo ""
  echo "[INFO] Searching for files in benchmark_metrics_*/work directories..."
  for metrics_dir in benchmark_metrics_*/; do
    if [ -d "$metrics_dir" ]; then
      echo ""
      echo "=== Found directory: $metrics_dir ==="

      work_dir="${metrics_dir}work"
      if [ -d "$work_dir" ]; then
        echo ""
        echo "=== Checking work directory: $work_dir ==="

        # Print all files in the work directory
        for file in "$work_dir"/*; do
          if [ -f "$file" ]; then
            echo ""
            echo "--- Contents of $file ---"
            cat "$file"
          fi
        done
      fi
    fi
  done
}
