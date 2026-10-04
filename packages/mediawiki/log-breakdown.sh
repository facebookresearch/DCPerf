#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Records the start or end of the measured phase in breakdown.csv. `run.sh -b`
# passes this script to perf.php through --exec-after-warmup and
# --exec-after-benchmark, which run immediately before and after the measured
# load-generator run.
# Usage: log-breakdown.sh <start|end>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/../common/runtime_breakdown_utils.sh"

: "${MEDIAWIKI_BREAKDOWN_FOLDER:?must be exported by run.sh -b}"

case "$1" in
  start)
    log_main_benchmark_start "${MEDIAWIKI_BREAKDOWN_FOLDER}" "${PPID}"
    ;;
  end)
    log_main_benchmark_end "${MEDIAWIKI_BREAKDOWN_FOLDER}" "${PPID}"
    ;;
  *)
    echo "Usage: ${0##*/} <start|end>" >&2
    exit 1
    ;;
esac
