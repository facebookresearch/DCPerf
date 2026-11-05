#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Install Silo
################################################################################

install_silo () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Install Silo"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[INSTALL] Installing Silo ..."
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py -b mem install silo_default) || return 1
}


################################################################################
# Run Silo smoke tests (TPC-C default + YCSB-C, all cores)
#
# The YCSB-C run is the regression check for the sequential-loader
# remainder bug: with scale-factor 1 (1000 keys) it only passes when
# every key is loaded, whatever the runner's core count is. The
# artifact check below mirrors the Skycastle Silo workflow, which
# parses the same metrics files.
################################################################################

run_silo_smoke_test () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run Silo Smoke Test"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[SMOKE] Running Silo TPC-C smoke test ..."
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py -b mem run silo_default) || return 1

  echo "[SMOKE] Running Silo YCSB-C smoke test ..."
  (print_exec conda run --no-capture-output ${env_prefix} \
    python ./benchpress_cli.py -b mem run silo_ycsb_c) || return 1

  echo "[SMOKE] Checking Silo metrics artifacts ..."
  python3 - <<'PY' || return 1
import glob
import json
import sys

found = {}
for path in glob.glob("benchmark_metrics_*/silo_*_metrics_*.json"):
    with open(path) as f:
        data = json.load(f)
    found[data["benchmark_name"]] = data["metrics"]

for job in ("silo_default", "silo_ycsb_c"):
    if job not in found:
        print("ERROR: no metrics artifact found for %s" % job, file=sys.stderr)
        sys.exit(1)
    metrics = found[job]
    throughput = metrics["throughput"]["agg_throughput"]
    if not throughput > 0:
        print(
            "ERROR: %s agg_throughput is not greater than zero: %s"
            % (job, throughput),
            file=sys.stderr,
        )
        sys.exit(1)
    if not metrics["txn_breakdown"]:
        print("ERROR: %s txn_breakdown is empty" % job, file=sys.stderr)
        sys.exit(1)
    print(
        "SUCCESS: %s agg_throughput = %s, txn_breakdown = %s"
        % (job, throughput, metrics["txn_breakdown"])
    )
PY
}
