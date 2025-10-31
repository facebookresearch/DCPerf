#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Run Mediawiki
################################################################################

run_mediawiki () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run Mediawiki"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")
  export DEBIAN_FRONTEND=noninteractive
  echo "[RUN] Running Mediawiki ..."
  # (print_exec conda run --no-capture-output ${env_prefix} \
  #  python ./benchpress_cli.py run oss_performance_mediawiki_mini) || return 1

   ./packages/mediawiki/run.sh \
     -r/usr/local/hphpi/legacy/bin/hhvm \
     -nnginx \
     -L wrk \
     -s benchmarks/oss_performance_mediawiki/wrk/wrk \
     -T default_no_temp_dir \
     -p -- \
     --mediawiki \
     --client-duration=4s \
     --client-timeout=60s \
     --run-as-root \
     --i-am-not-benchmarking \
     --shorten-health-check \
     --skip-single-request-warmup \
     --skip-sleep-between-warmups \
     --hhvm-extra-arguments="-vEval.JitRetranslateAllSeconds=5" \
     --num-multi-req-warmups=-1 \
     --no-load-if-pending-translate \
     --first-multi-warmup-duration=25 \
     --subseq-multi-warmup-duration=5 \
     --load-gen-seed=1000
}
