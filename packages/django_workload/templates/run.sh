#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Abs path of benchmarks/django_workload/bin
SCRIPT_ROOT="$(dirname "$(readlink -f "$0")")"
# Abs path to DCPerf root
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_ROOT}/../../..")"
MEMCACHED_PID=
CLEANUP_REQS=0

if [ -z "$JAVA_HOME" ]; then
  _JAVA_HOME="$("${BENCHPRESS_ROOT}"/packages/common/find_java_home.py)"
  export JAVA_HOME="${_JAVA_HOME}"
  echo "JAVA_HOME is not set, so setting it to ${JAVA_HOME}."
fi

# shellcheck disable=SC2317
cleanup() {
  echo "Stopping services ..."
  cd "${SCRIPT_ROOT}/.." || exit 1

  # Stop django-workload
  [ -f uwsgi.pid ] && { echo "Stopping uwsgi"; kill -INT "$(cat uwsgi.pid)" || true; }
  # Stop memcached
  [ -n "$MEMCACHED_PID" ] && { echo "Stopping memcached"; kill "$MEMCACHED_PID" || true; }
  # Stop Cassandra
  [ -f cassandra.pid ] && { echo "Stopping cassandra"; kill "$(cat cassandra.pid)" || true; }
  # Kill Siege
  SIEGE_PID="$(pgrep siege)"
  [ -n "$SIEGE_PID" ] && { echo "Killing siege"; kill -9 "$SIEGE_PID" || true; }
  echo "Done"
  if [ "$CLEANUP_REQS" -gt 0 ]; then
    exit
  fi
  CLEANUP_REQS=$((CLEANUP_REQS + 1))
}

trap 'cleanup' ERR EXIT SIGINT SIGTERM

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-r role] [-w number of workers] [-i number of iterations] [-d duration of workload] [-p number of repetitions] [-l siege logfile path] [-s urls path] [-c cassandra host ip]
Proxy shell script to executes django-workload benchmark
    -r          role (clientserver, client, server or db, default is clientserver)
    -h          display this help and exit
For role "server", "clientserver":
    -w          number of server workers (default NPROC)
    -c          ip address of the cassandra server (required)
    -m          minimum icachebuster calling rounds (default 100000)
    -M          maximum icachebuster calling rounds (default 200000)
For role "client", "clientserver":
    -x          number of client workers (default 1.2*NPROC)
    -i          number of iterations (default 7)
    -p          run each iteration of benchmark for fixed repetitions rather
                than certain amount of time. If this is set to a positive
                number, it will override "-d"
    -d          duration of django-workload benchmark (e.g. 2M)
    -l          path to log siege output to
    -s          source or path to get urls from
For role "client":
    -z          ip address of the django server (required when role is 'client', default is ::1)
For role "db":
    -y          number of cassandra concurrent writes (default 128)
    -b          ip address that cassandra will bind to (default to the first IP from "hostname -i": `hostname -i`)

EOF
}

run_benchmark() {
  core_factor=1.2
  local _num_workers=$1
  if [ "$_num_workers" -le 0 ] || [ -z "$_num_workers" ]; then
    _num_workers=$(echo "scale=2; $(nproc)*$core_factor" | bc) # this do decimal times
    _num_workers=$(echo "$_num_workers" | awk '{printf("%d\n",$1 + 0.5)}') # round to integer
  fi
  local _duration="$2"
  local _siege_logs_path="$3"
  local _urls_path="$4"
  local iterations="$5"
  local reps="$6"

  cd "${SCRIPT_ROOT}/../django-workload/client" || exit
  ./gen-urls-file
  WORKERS="$_num_workers" \
  DURATION="$_duration" \
  LOG="$_siege_logs_path" \
  SOURCE="$_urls_path" \
  python3 ./run-siege -i "${iterations}" -r "${reps}"
}

start_cassandra() {
  cd "${SCRIPT_ROOT}/.." || exit 1
  # Set the listening address
  local cassandra_concur_writes="$1"
  local cassandra_bind_addr="$2"
  if [ "$cassandra_concur_writes" -le 0 ] || [ -z "$cassandra_concur_writes" ]; then
    cassandra_concur_writes=128
  fi

  CASSANDRA_YAML="./apache-cassandra/conf/cassandra.yaml"
  if [ -z "$cassandra_bind_addr" ] || [ "$cassandra_bind_addr" = "default" ]; then
    HOST_IP="$(hostname -i | awk '{print $1}')"
  else
    HOST_IP="$cassandra_bind_addr"
  fi
  sed "s/__HOST_IP__/${HOST_IP}/g" < ${CASSANDRA_YAML}.template > ${CASSANDRA_YAML}.tmp
  sed "s/__CONCUR_WRITES__/${cassandra_concur_writes}/g" < ${CASSANDRA_YAML}.tmp > ${CASSANDRA_YAML}.tmp2
  mv -f "${CASSANDRA_YAML}.tmp2" "${CASSANDRA_YAML}"
  # Start Cassandra
  ./apache-cassandra/bin/cassandra -R -f -p cassandra.pid > cassandra.log 2>&1
}

start_django_server() {
  local cassandra_addr=$1
  local num_server_workers=$2

  # Start Memcached
  cd "${SCRIPT_ROOT}/.." || exit 1
  ./django-workload/services/memcached/run-memcached > memcached.log 2>&1 &
  MEMCACHED_PID=$!

  # Start django-workload
  # Set the cassandra ip in django config file
  cd "${SCRIPT_ROOT}/../django-workload/django-workload" || exit 1
  CLUSTER_SETTING="cluster_settings.py"
  sed -e "s/__CASSANDRA_DB_ADDR__/${cassandra_addr}/g" \
      < ${CLUSTER_SETTING}.template > ${CLUSTER_SETTING}.tmp
  mv -f "${CLUSTER_SETTING}.tmp" "${CLUSTER_SETTING}"

  # shellcheck disable=SC1090,SC1091
  source "${SCRIPT_ROOT}/../django-workload/django-workload/venv/bin/activate"

  # Wait for cassandra to start
  retries=60
  if ! nc -z "${cassandra_addr}" 9042; then
    echo "Waiting for Cassandra to start..."
    while ! nc -z "${cassandra_addr}" 9042; do
      sleep 1
      retries=$((retries-1))
      if [[ "$retries" -le 0 ]]; then
        echo "Cassandra could not start."
        exit 1
      fi
    done
    echo "Cassandra is ready."
  fi
  # Create database schema
  export LD_LIBRARY_PATH=${SCRIPT_ROOT}/../django-workload/django-workload/:${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
  DJANGO_SETTINGS_MODULE=cluster_settings ./venv/bin/django-admin flush
  DJANGO_SETTINGS_MODULE=cluster_settings ./venv/bin/django-admin setup

  echo "Running django server with ${num_server_workers} uWSGI workers"

  venv/bin/uwsgi \
    --ini uwsgi.ini \
    -H "${SCRIPT_ROOT}/../django-workload/django-workload/venv" \
    --safe-pidfile "${SCRIPT_ROOT}/../uwsgi.pid" \
    --workers "${num_server_workers}"
}

start_client() {
  local num_client_workers=$1
  local duration=$2
  local siege_logs_path=$3
  local urls_path=$4
  local server_addr=$5
  local iterations="$6"
  local reps="$7"

  # Replace the host in url template to the actual server addr
  CLIENTS_DIR="${SCRIPT_ROOT}/../django-workload/client"
  URLS_TEMPLATE="${CLIENTS_DIR}/urls_template.txt"
  sed -e "s/\/\/.*:8000/\/\/${server_addr}:8000/g" \
      < "${URLS_TEMPLATE}" > "${URLS_TEMPLATE}.tmp"
  mv -f "${URLS_TEMPLATE}.tmp" "${URLS_TEMPLATE}"

  # shellcheck disable=SC1090,SC1091
  source "${SCRIPT_ROOT}/../django-workload/django-workload/venv/bin/activate"

  run_benchmark "${num_client_workers}" "${duration}" "${siege_logs_path}" "${urls_path}" "${iterations}" "${reps}"
}

start_clientserver() {
  local cassandra_addr=$1
  local num_server_workers=$2
  local num_client_workers=$3
  local duration=$4
  local siege_logs_path=$5
  local urls_path=$6
  local iterations="$7"
  local reps="$8"

  start_django_server "${cassandra_addr}" "${num_server_workers}" &

  # Wait for the server to start
  local retries=150
  while ! nc -z localhost 8000; do
      sleep 1
      retries=$((retries-1))
      if [[ "$retries" -le 0 ]]; then
          echo "Django server could not start within 150s"
          exit 1
      fi
  done
  start_client "${num_client_workers}" "${duration}" "${siege_logs_path}" "${urls_path}" localhost "${iterations}" "${reps}"
}

main() {
  local num_server_workers
  num_server_workers="$(nproc)"

  local num_client_workers
  num_client_workers="0"

  local num_cassandra_writes
  num_cassandra_writes="128"

  local iterations
  iterations="7"

  local reps
  reps="0"

  local duration
  duration='2M'

  local siege_logs_path
  siege_logs_path='./siege.log'

  local urls_path
  urls_path='urls.txt'

  local role
  role='clientserver'

  local cassandra_addr
  cassandra_addr='::1'

  local server_addr
  server_addr='::1'

  local cassandra_bind_addr
  cassandra_bind_addr=''

  local django_ib_min
  django_ib_min="100000"

  local django_ib_max
  django_ib_max="200000"

  while getopts 'w:x:y:i:p:d:l:s:r:c:z:b:m:M:' OPTION "${@}"; do
    case "$OPTION" in
      w)
        # Use readlink to get absolute path if relative is given
        num_server_workers="${OPTARG}"
        ;;
      x)
        num_client_workers="${OPTARG}"
        ;;
      y)
        num_cassandra_writes="${OPTARG}"
        ;;
      i)
        iterations="${OPTARG}"
        ;;
      p)
        reps="${OPTARG}"
        ;;
      d)
        duration="${OPTARG}"
        ;;
      l)
        siege_logs_path="${OPTARG}"
        if [[ "$siege_logs_path" != './siege.log' ]]; then
          siege_logs_path="$(readlink -f "$siege_logs_path")"
        fi
        ;;
      s)
        urls_path="${OPTARG}"
        if [[ "$urls_path" != 'urls.txt' ]]; then
          urls_path="$(readlink -f "$urls_path")"
        fi
        ;;
      r)
        role="${OPTARG}"
        ;;
      c)
        cassandra_addr="${OPTARG}"
        ;;
      z)
        server_addr="${OPTARG}"
        ;;
      b)
        cassandra_bind_addr="${OPTARG}"
        ;;
      m)
        django_ib_min="${OPTARG}"
        ;;
      M)
        django_ib_max="${OPTARG}"
        ;;
      ?)
        show_help >&2
        exit 1
        ;;
    esac
  done
  shift "$((OPTIND - 1))"

  readonly num_server_workers
  readonly num_client_workers
  readonly num_cassandra_writes
  readonly iterations
  readonly reps
  readonly duration
  readonly siege_logs_path
  readonly urls_path
  readonly role
  readonly cassandra_addr
  readonly server_addr
  readonly cassandra_bind_addr
  readonly django_ib_min
  readonly django_ib_max

  if [ "$role" = "db" ]; then
    start_cassandra "$num_cassandra_writes" "$cassandra_bind_addr";
  elif [ "$role" = "clientserver" ]; then
    export IB_MIN="${django_ib_min}"
    export IB_MAX="${django_ib_max}"
    start_clientserver "$cassandra_addr" "$num_server_workers" "$num_client_workers" "$duration" "$siege_logs_path" "$urls_path" "$iterations" "$reps";
  elif [ "$role" = "client" ]; then
    start_client "$num_client_workers" "$duration" "$siege_logs_path" "$urls_path" "$server_addr" "$iterations" "$reps";
  elif [ "$role" = "server" ]; then
    export IB_MIN="${django_ib_min}"
    export IB_MAX="${django_ib_max}"
    start_django_server "$cassandra_addr" "$num_server_workers";
  elif [ "$role" = "standalone" ]; then
    export IB_MIN="${django_ib_min}"
    export IB_MAX="${django_ib_max}"
    start_cassandra "$num_cassandra_writes" 127.0.0.1 &
    start_clientserver "$cassandra_addr" "$num_server_workers" "$num_client_workers" "$duration" "$siege_logs_path" "$urls_path" "$iterations" "$reps";
    pgrep -f cassandra | xargs kill

  else
    echo "Role $role is invalid, it can only be 'db' or 'clientserver' or 'standalone'";
    exit 1
  fi
  exit 0
}

main "$@"
