#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.


# shellcheck disable=SC1091,SC2128
. "$( dirname -- "$BASH_SOURCE"; )/utils_base.bash"


################################################################################
# Django Run Functions
################################################################################

run_django () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Run Django"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi


  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[RUN] Running Django ..."

  # (print_exec conda run --no-capture-output ${env_prefix} \
  #  python ./benchpress_cli.py run django_workload_default -r standalone -i "'{\"reps\": 1000, \"iterations\": 1}'") || return 1
  ./benchmarks/django_workload/bin/run.sh -r standalone -d 5M -i 1 p 1000 -l /siege.log -s urls.txt -c 127.0.0.1 -I cpython || true
  echo "PWD"
  echo $PWD
  echo "Searching for django-uwsgi.log files:"
  find_output=$(find . -name django-uwsgi.log)
  echo "$find_output"
  # cat django-workload/django-workload/django-uwsgi.log
}
