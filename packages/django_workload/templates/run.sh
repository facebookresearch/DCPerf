#!/bin/bash

SCRIPT_ROOT="$(dirname "$(readlink -f "$0")")"
MEMCACHED_PID=

if [ -z "$JAVA_HOME" ]; then
    export JAVA_HOME="/etc/alternatives/jre_1.8.0_openjdk"
fi

cleanup() {
  echo "Stopping services ..."
  cd "${SCRIPT_ROOT}/.." || exit 1

  # Stop django-workload
  [ -f uwsgi.pid ] && { echo "Stopping uwsgi"; kill -INT "$(cat uwsgi.pid)" || true; }
  # Stop memcached
  [ -n "$MEMCACHED_PID" ] && { echo "Stopping memcached"; kill "$MEMCACHED_PID" || true; }
  # Stop Cassandra
  [ -f cassandra.pid ] && { echo "Stopping cassandra"; kill "$(cat cassandra.pid)" || true; }
  echo "Done"
}

trap 'cleanup' ERR EXIT SIGINT SIGTERM

show_help() {
cat <<EOF
Usage: ${0##*/} [-h] [-r role] [-w number of workers] [-d duration of workload] [-l siege logfile path] [-s urls path] [-c cassandra host ip]
Proxy shell script to executes django-workload benchmark
    -r          role (clientserver or db, default is clientserver)
    -h          display this help and exit
    -w          number of server workers (default NPROC)
    -x          number of client workers (default 1.2*NPROC)
    -y          number of cassandra concurrent writes (default 128)
    -d          duration of django-workload benchmark (e.g. 2M)
    -l          path to log siege output to
    -s          source or path to get urls from
    -c          ip address of the cassandra server

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

  cd "${SCRIPT_ROOT}/../django-workload/client" || exit
  ./gen-urls-file
  WORKERS="$_num_workers" \
  DURATION="$_duration" \
  LOG="$_siege_logs_path" \
  SOURCE="$_urls_path" \
  python3 ./run-siege
}

start_cassandra() {
  cd "${SCRIPT_ROOT}/.." || exit 1
  # Set the listening address
  local cassandra_concur_writes="$1"
  if [ "$cassandra_concur_writes" -le 0 ] || [ -z "$cassandra_concur_writes" ]; then
    cassandra_concur_writes=128
  fi

  CASSANDRA_YAML="./apache-cassandra/conf/cassandra.yaml"
  HOST_IP="$(hostname -i | awk '{print $1}')"
  sed "s/__HOST_IP__/${HOST_IP}/g" < ${CASSANDRA_YAML}.template > ${CASSANDRA_YAML}.tmp
  sed "s/__CONCUR_WRITES__/${cassandra_concur_writes}/g" < ${CASSANDRA_YAML}.tmp > ${CASSANDRA_YAML}.tmp2
  mv -f "${CASSANDRA_YAML}.tmp2" "${CASSANDRA_YAML}"
  # Start Cassandra
  ./apache-cassandra/bin/cassandra -R -f -p cassandra.pid > cassandra.log 2>&1
}

start_clientserver() {
  local cassandra_addr=$1
  local num_server_workers=$2
  local num_client_workers=$3
  local duration=$4
  local siege_logs_path=$5
  local urls_path=$6

  # Start Memcached
  cd "${SCRIPT_ROOT}/.." || exit 1
  ./django-workload/services/memcached/run-memcached > memcached.log 2>&1 &
  MEMCACHED_PID=$!

  # Start django-workload
  # Set the cassandra ip in django config file
  cd "${SCRIPT_ROOT}/../django-workload/django-workload" || exit 1
  CLUSTER_SETTING="cluster_settings.py"
  sed "s/__CASSANDRA_DB_ADDR__/${cassandra_addr}/g" < ${CLUSTER_SETTING}.template > ${CLUSTER_SETTING}.tmp
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

  venv/bin/uwsgi \
    --ini uwsgi.ini \
    -H "${SCRIPT_ROOT}/../django-workload/django-workload/venv" \
    --safe-pidfile "${SCRIPT_ROOT}/../uwsgi.pid" \
    --workers "${num_server_workers}" &

  echo "Server workers: ${num_server_workers}; client workers: ${num_client_workers}"
  run_benchmark "${num_client_workers}" "${duration}" "${siege_logs_path}" "${urls_path}"
}

main() {
  local num_server_workers
  num_server_workers="$(nproc)"

  local num_client_workers
  num_client_workers="0"

  local num_cassandra_writes
  num_cassandra_writes="128"

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

  while getopts 'w:x:y:d:l:s:r:c:' OPTION "${@}"; do
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
  readonly duration
  readonly siege_logs_path
  readonly urls_path
  readonly role
  readonly cassandra_addr

  if [ "$role" = "db" ]; then
    start_cassandra "$num_cassandra_writes";
  elif [ "$role" = "clientserver" ]; then
    start_clientserver "$cassandra_addr" "$num_server_workers" "$num_client_workers" "$duration" "$siege_logs_path" "$urls_path";
  else
    echo "Role $role is invalid, it can only be 'db' or 'clientserver'";
    exit 1
  fi
  exit 0
}

main "$@"
