#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
declare OLD_CWD
OLD_CWD="$( pwd )"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &>/dev/null && pwd -P)"

BC_MAX_FN='define max (a, b) { if (a >= b) return (a); return (b); }'
NPROC="$(nproc)"
HHVM_SERVERS="$(( (NPROC + 99) / 100 ))"
SERVER_THREADS=$(echo "${BC_MAX_FN}; max(200, (2.8 * ${NPROC}) / ${HHVM_SERVERS})" | bc)  # Divides by integer 1 to truncate decimal
SIEGE_CLIENT_THREADS=$(echo "${BC_MAX_FN}; max(200, (150 * ${HHVM_SERVERS}))" | bc)
WRK_CLIENT_THREADS=$(( 2 * NPROC ))
MEMCACHE_THREADS=8

export LD_LIBRARY_PATH=/opt/local/hhvm-3.30/lib

# Function to detect if running in Docker container
is_docker_container() {
  # Check for .dockerenv file (most reliable method)
  if [ -f /.dockerenv ]; then
    return 0
  fi
  return 1
}

# Function to restart MariaDB in Docker container
restart_mariadb_docker() {
  echo "Restarting MariaDB in Docker container mode..."
  pkill mariadb
  # Wait until MariaDB is fully killed before starting it again
  while pgrep -f mariadb > /dev/null; do
    sleep 1
  done
  # Start MariaDB in the background with nohup to ensure it's fully detached
  nohup mariadbd --user=mysql --socket=/var/lib/mysql/mysql.sock > /dev/null 2>&1 &
}

# Function to restart MariaDB on bare-metal machine
restart_mariadb_systemctl() {
  echo "Restarting MariaDB using systemctl..."
  systemctl restart mariadb
}

function show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-H db host] [-r hhvm path] [-n nginx path] [-L siege or wrk ] [-s load generator path] [-t server threads] [-c client threads] [-m memcache thrads] [-p] [-T temp-dir] [-- extra_args]
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
    -p          disable perf-record.sh execution after warmup
    -T          specify temporary directory path
    -j          enable JIT size monitoring with dump_jit_size.py
    -J          JIT monitoring port (default: localhost:9092)
    -I          JIT monitoring interval in seconds (default: 1)
    -D          enable TC dump with vm-dump-tc
    -C          TC dump interval in seconds (default:600)
    -O          output directory for JIT monitoring files (default: /tmp/jit_study_output)

Any other options that oss-performance perf.php script could accept can be
passed in as extra arguments appending two hyphens '--' followed by the
arguments. Example:

${0##*/} -- --mediawiki --wrk-duration 10m --exec-after-benchmark time

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
    if is_docker_container; then
      status="$(ps aux | grep ${service} > /dev/null && echo "active" || echo "no active")"
    else
      status="$(systemctl show ${service} | awk -F= '/ActiveState/{print $2}')"
    fi

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

# Start JIT monitoring in the background
function start_jit_monitoring() {
  local ip_and_port="$1"
  local dump_jit_interval="$2"
  local dump_tc_enabled="$3"
  local dump_tc_interval="$4"
  local output_dir="$5"

  # Build command with optional TC dump parameters
  local jit_monitor_cmd="python3 ${SCRIPT_DIR}/dump_jit_size.py \
    --output-dir \"${output_dir}\" \
    --dump-jit-interval \"${dump_jit_interval}\" \
    --ip-and-port \"${ip_and_port}\""

  # Add TC dump options if enabled
  if [[ "$dump_tc_enabled" == "true" ]]; then
    jit_monitor_cmd="${jit_monitor_cmd} \
    --dump-tc \
    --dump-tc-interval \"${dump_tc_interval}\""
    echo "TC dump enabled with interval: ${dump_tc_interval} seconds"
  fi

  echo "Starting JIT size monitoring..."
  echo "JIT monitoring output directory: ${output_dir}"

  # Execute the command
  eval "${jit_monitor_cmd}" &

  # Save the PID for later cleanup
  JIT_MONITOR_PID=$!
  echo "JIT monitoring started with PID: ${JIT_MONITOR_PID}"
}

# Stop JIT size monitoring
function stop_jit_monitoring() {
  if [[ -n "${JIT_MONITOR_PID}" ]]; then
    echo "Stopping JIT size monitoring (PID: ${JIT_MONITOR_PID})..."
    kill "${JIT_MONITOR_PID}" 2>/dev/null || true
    wait "${JIT_MONITOR_PID}" 2>/dev/null || true
    echo "JIT size monitoring stopped."
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
  local _disable_perf_record="$6"
  local _use_temp_dir="$7"
  local _temp_dir="$8"
  local _perf_record_arg=""
  local _temp_dir_arg=""

  if [[ "$_disable_perf_record" != "true" ]]; then
    _perf_record_arg="--exec-after-warmup=${SCRIPT_DIR}/perf-record.sh"
  fi

  if [[ "$_use_temp_dir" = true && "$_temp_dir" != "default_no_temp_dir" ]]; then
    _temp_dir_arg="--temp-dir ${_temp_dir}"
  fi

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
  if [ "${CLIENT_THREADS}" -gt 0 ]; then
    client_threads="${CLIENT_THREADS}"
  fi

  # Start JIT monitoring if enabled
  if [[ "${enable_jit_monitoring}" == "true" ]]; then
    start_jit_monitoring "${jit_monitor_port}" "${jit_monitor_interval}" "${enable_tc_dump}" "${tc_dump_interval}" "${jit_output_dir}"
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
    ${_perf_record_arg} \
    ${_temp_dir_arg} \
    ${extra_args}

  local benchmark_exit_code=$?

  # Stop JIT monitoring if it was started
  if [[ "${enable_jit_monitoring}" == "true" ]]; then
    stop_jit_monitoring
  fi

  cd "${OLD_CWD}" || exit
  return $benchmark_exit_code
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

  local disable_perf_record
  disable_perf_record=false

  local temp_dir
  temp_dir=""

  # JIT monitoring options
  enable_jit_monitoring=false
  jit_monitor_port="localhost:9092"
  jit_monitor_interval=1
  enable_tc_dump=false
  tc_dump_interval=600
  jit_output_dir="/tmp/jit_study_output"

  while getopts 'H:n:r:L:s:R:t:c:m:pT:jJ:DI:C:O:' OPTION "${@}"; do
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
      p)
        disable_perf_record=true
        ;;
      T)
        temp_dir="${OPTARG}"
        use_temp_dir=true
        ;;
      j)
        enable_jit_monitoring=true
        ;;
      J)
        jit_monitor_port="${OPTARG}"
        ;;
      I)
        jit_monitor_interval="${OPTARG}"
        ;;
      D)
        enable_tc_dump=true
        ;;
      C)
        tc_dump_interval="${OPTARG}"
        ;;
      O)
        jit_output_dir="${OPTARG}"
        # Make sure the directory exists
        mkdir -p "${jit_output_dir}"
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
  readonly disable_perf_record
  readonly use_temp_dir
  readonly temp_dir
  readonly enable_jit_monitoring
  readonly jit_monitor_port
  readonly jit_monitor_interval
  readonly enable_tc_dump
  readonly tc_dump_interval
  readonly jit_output_dir

  # Check if dump_jit_size.py exists when JIT monitoring is enabled
  if [[ "${enable_jit_monitoring}" == "true" ]]; then
    if [[ ! -f "${SCRIPT_DIR}/dump_jit_size.py" ]]; then
      echo "Error: dump_jit_size.py not found in ${SCRIPT_DIR}"
      exit 1
    fi

    # Make sure the script is executable
    chmod +x "${SCRIPT_DIR}/dump_jit_size.py"

    echo "JIT monitoring enabled:"
    echo "  - Server: ${jit_monitor_port}"
    echo "  - JIT monitoring interval: ${jit_monitor_interval} seconds"
    echo "  - Output directory: ${jit_output_dir}"

    if [[ "${enable_tc_dump}" == "true" ]]; then
      echo "  - TC dumping enabled with interval: ${tc_dump_interval} seconds"
    fi
  fi

  echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse

  if [[ "$db_host" = "" ]]; then
    # Restart MariaDB using appropriate method based on environment
    if is_docker_container; then
      restart_mariadb_docker
    else
      restart_mariadb_systemctl
    fi
    _check_local_db_running || return
    run_benchmark "${hhvm_path}" "${nginx_path}" "${load_generator}" "${lg_path}" "" "${disable_perf_record}" "${use_temp_dir}" "${temp_dir}"
  else
    run_benchmark "${hhvm_path}" "${nginx_path}" "${load_generator}" "${lg_path}" "${db_host}" "${disable_perf_record}" "${use_temp_dir}" "${temp_dir}"
  fi

  exit 0
}

# Make sure to clean up JIT monitoring on script exit
function cleanup() {
  stop_jit_monitoring
  cd "${OLD_CWD}" || exit 1
  exit 1
}

# shellcheck disable=2064,2172
trap cleanup 1 2 3 13 15

main "$@"
