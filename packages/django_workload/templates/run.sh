#!/bin/bash

SCRIPT_ROOT="$(dirname "$(readlink -f "$0")")"


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
Usage: ${0##*/} [-h] [-w number of workers] [-d duration of workload] [-l siege logfile path] [-s urls path]
Proxy shell script to executes django-workload benchmark
    -h          display this help and exit
    -w          number of workers
    -d          duration of django-workload benchmark (e.g. 2M)
    -l          path to log siege output to
    -s          source or path to get urls from

EOF
}

run_benchmark() {
  core_factor=1.2
  local _num_workers=$1
  _num_workers=$(echo "scale=2; $(nproc)*$core_factor" | bc) # this do decimal times
  _num_workers=$(echo "$_num_workers" | awk '{printf("%d\n",$1 + 0.5)}') # round to integer
  local _duration="$2"
  local _siege_logs_path="$3"
  local _urls_path="$4"

  cd "${SCRIPT_ROOT}/../django-workload/client" || exit
  ./gen-urls-file
  WORKERS="$_num_workers" \
  DURATION="$_duration" \
  LOG="$_siege_logs_path" \
  SOURCE="$_urls_path" \
  ./run-siege
}

main() {
  local num_workers
  num_workers='144'

  local duration
  duration='2M'

  local siege_logs_path
  siege_logs_path='./siege.log'

  local urls_path
  urls_path='urls.txt'

  while getopts 'w:d:l:s:' OPTION "${@}"; do
    case "$OPTION" in
      w)
        # Use readlink to get absolute path if relative is given
        num_workers="${OPTARG}"
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
      ?)
        show_help >&2
        exit 1
        ;;
    esac
  done
  shift "$((OPTIND - 1))"

  readonly num_workers
  readonly duration
  readonly siege_logs_path
  readonly urls_path

  cd "${SCRIPT_ROOT}/.." || exit 1
  # Start Cassandra
  ./apache-cassandra/bin/cassandra -R -p cassandra.pid > cassandra.log 2>&1

  # Start Memcached
  ./django-workload/services/memcached/run-memcached > memcached.log 2>&1 &
  MEMCACHED_PID=$!

  # Start django-workload
  cd "${SCRIPT_ROOT}/../django-workload/django-workload" || exit 1
  # shellcheck disable=SC1090,SC1091
  source "${SCRIPT_ROOT}/../django-workload/django-workload/venv/bin/activate"

  # Wait for cassandra to start
  retries=60
  if ! nc -z localhost 9042; then
    echo "Waiting for Cassandra to start..."
    while ! nc -z localhost 9042; do
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
    --workers "$(nproc)" &

  echo "${num_workers}"
  run_benchmark "${num_workers}" "${duration}" "${siege_logs_path}" "${urls_path}"


  exit 0
}

main "$@"
