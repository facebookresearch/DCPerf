#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
declare OLD_CWD
OLD_CWD="$( pwd )"

BC_MAX_FN='define max (a, b) { if (a >= b) return (a); return (b); }'
NPROC="$(nproc)"
HHVM_SERVERS="$(( (NPROC + 99) / 100 ))"
SERVER_THREADS=$(echo "${BC_MAX_FN}; max(200, (2.8 * $(nproc)) / ${HHVM_SERVERS})" | bc)  # Divides by integer 1 to truncate decimal
SIEGE_CLIENT_THREADS=$(echo "${BC_MAX_FN}; max(200, (150 * ${HHVM_SERVERS}))" | bc)
WRK_CLIENT_THREADS=$(( 2 * NPROC ))
MEMCACHE_THREADS=$(( 8 * HHVM_SERVERS ))

export LD_LIBRARY_PATH=/opt/local/hhvm-3.30/lib

function show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-H db host] [-r hhvm path] [-n nginx path] [-L siege or wrk ] [-s load generator path] [-t server threads] [-c client threads] [-m memcache thrads] [-- extra_args]
Proxy shell script to executes oss-performance benchmark
    -h          display this help and exit
    -H          hostname or IP address to mariadb or mysql database
    -n          path to nginx binary (default: 'nginx')
    -r          path to hhvm binary (default: 'hhvm')
    -L          load generator type (needs to be 'siege' or 'wrk')
    -s          path to the load generator
(For the next three parameters, 0 implies using default)
    -R          number of HHVM server instances. Default: ceil(logical cpus / 100) (=${HHVM_SERVERS})
    -t          number of server threads for each HHVM.
                Default: 200 or floor(2.8 * logical cpus / number of HHVM servers), whichever is greater (=${SERVER_THREADS})
    -c          number of load generator threads. Default: ${SIEGE_CLIENT_THREADS} for siege, ${WRK_CLIENT_THREADS} for wrk.
    -m          number of memcache threads. Default: 8 * number of HHVM (=${MEMCACHE_THREADS})

Any other options that oss-performance perf.php script could accept can be
passed in as extra arguments appending two hyphens '--' followed by the
arguments. Example:

${0##*/} -- --mediawiki --wrk-duration 600 --exec-after-benchmark time

EOF
}

# Check either mariadb or mysql is running
# Assuming systemd
# Note that if mariadb or mysql is not running or is not installed
# ActiveState will still show 'inactive'
function _systemd_service_status() {
  local service="$1"

  local status
  # shellcheck disable=2086
  status="$(systemctl show ${service} | awk -F= '/ActiveState/{print $2}')"
  echo "$status"
}

function _check_local_db_running() {
  local mariadb_status
  mariadb_status="$(_systemd_service_status mariadb)"

  local mysql_status
  mysql_status="$(_systemd_service_status mysqld)"

  if [[ "$mariadb_status" != "active" ]] && [[ "$mysql_status" != "active" ]]
  then
    >&2 echo "Make sure either 'mariadb' or 'mysql' is running."
    return 1
  fi
}

# Executes the oss-benchmark
# run_benchmark hhvm_path nginx_path wrk_path [db_host]
function run_benchmark() {
  local _hhvm_path="$1"
  local _nginx_path="$2"
  local _load_generator="$3"
  local _lg_path="$4"
  local _db_host=""

  if [[ $# -eq 5 ]]; then
    _db_host="--db-host $5"
  fi
  lg_params=""
  client_threads=0
  if [[ "$_load_generator" = "siege" ]]; then
    lg_params="--siege ${_lg_path}"
    client_threads="${SIEGE_CLIENT_THREADS}"
  elif [[ "$_load_generator" = "wrk" ]]; then
    lg_params="--wrk ${_lg_path}"
    client_threads="${WRK_CLIENT_THREADS}"
  fi

  cd "${OLD_CWD}/oss-performance" || exit
  # shellcheck disable=2086
  HHVM_DISABLE_NUMA=1 "$_hhvm_path" \
    -vEval.ProfileHWEnable=0 \
    perf.php \
    --nginx "$_nginx_path" \
    ${lg_params} \
    --hhvm "$_hhvm_path" \
    ${_db_host} \
    --db-username=root \
    --db-password=password \
    --memcached=/usr/local/memcached/bin/memcached \
    --memcached-threads "$MEMCACHE_THREADS" \
    --client-threads "${client_threads}" \
    --server-threads "$SERVER_THREADS" \
    --scale-out "${HHVM_SERVERS}" \
    --delay-check-health 30 \
    --hhvm-extra-arguments='-vEval.ProfileHWEnable=0' \
    ${extra_args}
  cd "${OLD_CWD}" || exit
}

function main() {
  local db_host
  db_host=""

  local hhvm_path
  hhvm_path='hhvm'

  local nginx_path
  nginx_path='nginx'

  local load_generator
  load_generator=''

  local lg_path
  lg_path=''

  while getopts 'H:n:r:L:s:R:t:c:m:' OPTION "${@}"; do
    case "$OPTION" in
      H)
        db_host="${OPTARG}"
        ;;
      n)
        # Use readlink to get absolute path if relative is given
        nginx_path="${OPTARG}"
        if [[ "$nginx_path" != 'nginx' ]]; then
          nginx_path="$(readlink -f "$nginx_path")"
        fi
        ;;
      r)
        hhvm_path="${OPTARG}"
        if [[ "$hhvm_path" != 'hhvm' ]]; then
          hhvm_path="$(readlink -f "$hhvm_path")"
        fi
        ;;
      L)
        load_generator="${OPTARG}"
        if ! [[ "$load_generator" = 'wrk' ]] && ! [[ "$load_generator" = 'siege' ]]; then
          echo "Load generator (-L) must be either 'wrk' or 'siege'."
          exit 1
        fi
        ;;
      s)
        lg_path="${OPTARG}"
        lg_path="$(which ${lg_path})"
        lg_path="$(readlink -f "${lg_path}")"
        if ! [[ -x "$lg_path" ]]; then
          echo "Specified load generator ${lg_path} is not an executable or does not exist."
          exit 1
        fi
        ;;
      R)
        if [[ "${OPTARG}" -gt 0 ]]; then
          HHVM_SERVERS="${OPTARG}"
        fi
        ;;
      t)
        if [[ "${OPTARG}" -gt 0 ]]; then
          SERVER_THREADS="${OPTARG}"
        fi
        ;;
      c)
        if [[ "${OPTARG}" -gt 0 ]]; then
          CLIENT_THREADS="${OPTARG}"
        fi
        ;;
      m)
        MEMCACHE_THREADS="${OPTARG}"
        ;;
      ?)
        show_help >&2
        exit 1
        ;;
    esac
  done
  shift "$((OPTIND -1))"

  # Extra arguments to pass to perf.php
  # shellcheck disable=2124
  extra_args=$@

  readonly db_host
  readonly hhvm_path
  readonly nginx_path
  readonly load_generator
  readonly lg_path

  echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse

  if [[ "$db_host" = "" ]]; then
    systemctl restart mariadb
    _check_local_db_running || return
    run_benchmark "${hhvm_path}" "${nginx_path}" "${load_generator}" "${lg_path}"
  else
    run_benchmark "${hhvm_path}" "${nginx_path}" "${load_generator}" "${lg_path}" "${db_host}"
  fi

  exit 0
}

# shellcheck disable=2064,2172
trap "cd ${OLD_CWD}; exit 1" 1 2 3 13 15

main "$@"
