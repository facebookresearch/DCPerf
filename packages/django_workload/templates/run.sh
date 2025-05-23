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
TABLE_NAMES="bundle_entry_model bundle_seen_model feed_entry_model inbox_entries user_model"
CASSANDRA_DATA_PATH="/data/cassandra/data"
KEY_SPACE_NAME="db"
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
    -L          when provided snapshot loading is enabled, meaning that the database is loaded from a snapshot stored in the specifed path (default disabled)
    -t          when provided snapshot taking is enabled, meaning that the a snapshot of the generetaed database will be stored in the specifed path (default disabled)

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

collect_perf_record() {
    sleep 30
    if [ -f "perf.data" ]; then
        return 0
    fi
    perf record -a -g -- sleep 5 >> /tmp/perf-record.log 2>&1
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

  if [ "${DCPERF_PERF_RECORD}" = 1 ] && ! [ -f "perf.data" ]; then
      collect_perf_record &
  fi

  WORKERS="$_num_workers" \
  DURATION="$_duration" \
  LOG="$_siege_logs_path" \
  SOURCE="$_urls_path" \
  python3 ./run-siege -i "${iterations}" -r "${reps}"
}

load_snapshot(){
  extra_options="-h ${cassandra_addr_IPV6}" # the IPV^ is essential for the standlone version, wehre we pass IPV4 version as cassandra_IP. This line is equievalnet to this :extra_options="-h ::FFFF:127.0.0.1
  #https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/operations/opsBackupSnapshotRestore.html
  # for nodetool V3 there is no import command. So, according to the mentioned documentaion, we need to copy the snapshot files to the corresponding table folders
  echo "Loading snapshot from ${snapshot_dir} to ${CASSANDRA_DATA_PATH}/${KEY_SPACE_NAME} "
  source_directory=${snapshot_dir}
  dest_directory=${CASSANDRA_DATA_PATH}/${KEY_SPACE_NAME}

  # Check if destination directory exists
  if [ ! -d "$source_directory" ]; then
    echo "Error: $source_directory does not exist"
    exit 1
  fi

  # For each table, check if there's a directory in source_directory that starts with the table name
  # If so, copy its content to the corresponding directory in dest_directory
  for table in ${TABLE_NAMES}; do
    # Find directories in source_directory that start with the table name
    for src_dir in ${source_directory}/${table}*/; do
      if [ -d "$src_dir" ]; then
        # Find the corresponding directory in dest_directory
        for dst_dir in ${dest_directory}/${table}*/; do
          if [ -d "$dst_dir" ]; then
            echo "Copying content from ${src_dir} to ${dst_dir}"
            cp -rf "${src_dir}"* "${dst_dir}" || echo "Failed to copy from ${src_dir} to ${dst_dir}"
          fi
        done
      fi
    done
  done

   # Loop through each table in TABLE_NAMES and refresh it
  #echo "Refreshing tables in ${KEY_SPACE_NAME}..."
  for table in ${TABLE_NAMES}; do
    #echo "Refreshing table: ${table}"
    ${BENCHPRESS_ROOT}/benchmarks/django_workload/apache-cassandra/bin/nodetool ${extra_options} refresh -- ${KEY_SPACE_NAME} ${table} || exit 1
  done
  # The documentation says that the node should be restarted but it is not necessary and the node frrezes sometimes if we restart it, So for now the restart code is commented until we are sure that the restart is unecessary
  #pgrep -f cassandra | xargs kill
  #./apache-cassandra/bin/cassandra -R -f -p cassandra.pid > cassandra.log 2>&1 & #Send to the background doese not work this way
  #wait_for_cassandra_to_start


  echo "End of loading snapshot ..."
}
wait_for_cassandra_to_start() {
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
}

take_snapshot(){
  snapshot_name=$(basename "${snapshot_dir}")
  for table_dir in ${CASSANDRA_DATA_PATH}/${KEY_SPACE_NAME}/*/; do
    # The nodetool returns an error if the snapshot already exists, so we have to remove the existing one
    if [ -d "${table_dir}snapshots/${snapshot_name}" ]; then
      echo "The ${snapshot_name} exists in the ${CASSANDRA_DATA_PATH}/${KEY_SPACE_ANEM}. So, we are removing it to take a new snashopt"
      rm -rf ${table_dir}snapshots/${snapshot_name}
    fi
  done
  echo "Taking snapshot ..."

  #extra_options="-h ::FFFF:127.0.0.1 -p 7199" # the documention says this port should be used, but it did not work
  #extra_options="-h ::FFFF:127.0.0.1 -p 9042" # this port also is tried, but it did not work
  extra_options="-h ${cassandra_addr_IPV6}"
  command_options="-t ${snapshot_name}"
  # Our nodetool is v3, so unfortunately the new nodetool commands for taking a snapshot and importing a snapshot does not work, so we need to use the following commands based on the following documentation
  #https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/operations/opsBackupTakesSnapshot.html
  ${BENCHPRESS_ROOT}/benchmarks/django_workload/apache-cassandra/bin/nodetool ${extra_options}  cleanup ${KEY_SPACE_NAME} || exit 1
  ${BENCHPRESS_ROOT}/benchmarks/django_workload/apache-cassandra/bin/nodetool ${extra_options}  snapshot ${KEY_SPACE_NAME} ${command_options} || exit 1


  # The snapshot taken by the nodetool is scattered among several directories and has no option for storing in a user specified folder
  # So, we need to collect all the folders of a snapshot that the nodetool creates and move it to a user specifed path or the default path
  # This way, the snapshot will be stored in one folder that can easily be moved and be reused
  # Copy snapshot files to a new directory
  for table_dir in ${CASSANDRA_DATA_PATH}/${KEY_SPACE_NAME}/*/; do
    table_dir_name=$(basename "${table_dir}")
    mkdir -p "${snapshot_dir}/${table_dir_name}" || exit 1
    cp -rf ${table_dir}snapshots/${snapshot_name}/* ${snapshot_dir}/${table_dir_name}/ || exit 1
  done
  echo "The snapshot is stored in ${snapshot_dir} "
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

  wait_for_cassandra_to_start

  # Create database schema
  export LD_LIBRARY_PATH=${SCRIPT_ROOT}/../django-workload/django-workload/:${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    if [ "$load_a_snapshot" = true ]; then
      #DJANGO_SETTINGS_MODULE=cluster_settings ./venv/bin/django-admin flush
      load_snapshot
      # we need to restart Cassandra after loaing an snapshot
      echo "Cassandra is loaded using the snapshot"
    else
      echo "Generating database "
      DJANGO_SETTINGS_MODULE=cluster_settings ./venv/bin/django-admin flush
      DJANGO_SETTINGS_MODULE=cluster_settings ./venv/bin/django-admin setup

    fi
    if [ "$take_a_snapshot" = true ]; then
      take_snapshot
    fi
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
  local retries_init=150
  local retries=$retries_init
  while ! nc -z localhost 8000; do
      sleep 1
      retries=$((retries-1))
      if [[ "$retries" -le 0 ]]; then
          echo "Django server could not start within ${retries_init}s"
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

  local cassandra_addr
  cassandra_addr_IPV6=''

  local server_addr
  server_addr='::1'

  local cassandra_bind_addr
  cassandra_bind_addr=''

  local django_ib_min
  django_ib_min="100000"

  local django_ib_max
  django_ib_max="200000"

  local take_a_snapshot
  take_a_snapshot=false

  local load_a_snapshot
  load_a_snapshot=false

  local snapshot_dir
  snapshot_dir="${BENCHPRESS_ROOT}/benchmarks/django_workload/cassandra_snapshots/synthetic_dataset_snapshot"

  while getopts 'w:x:y:i:p:d:l:s:r:c:z:b:m:M:L:t:' OPTION "${@}"; do
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
      t)
        take_a_snapshot=true
        snapshot_dir="${OPTARG}"
        # Check if snapshot_dir is a relative path and prepend BENCHPRESS_ROOT if it is
        if [[ ! "${snapshot_dir}" = /* ]]; then
          snapshot_dir="${BENCHPRESS_ROOT}/${snapshot_dir}"
        fi
        ;;

      L)
        load_a_snapshot=true
        snapshot_dir="${OPTARG}"
        # Check if snapshot_dir is a relative path and prepend BENCHPRESS_ROOT if it is
        if [[ ! "${snapshot_dir}" = /* ]]; then
          snapshot_dir="${BENCHPRESS_ROOT}/${snapshot_dir}"
        fi
        # Check if snapshot_dir exists
        if [ ! -d "$snapshot_dir" ]; then
          echo "Error: Snapshot directory $snapshot_dir does not exist"
          exit 1
        fi
        ;;
      ?)
        show_help >&2
        exit 1
        ;;
    esac
  done
  shift "$((OPTIND - 1))"

  # Check if $cassandra_addr is in IPv4 format, if so convert to IPv6 format
  if [[ $cassandra_addr =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    # IPv4 format detected, convert to IPv6 format
    cassandra_addr_IPV6="::FFFF:$cassandra_addr"
  else
    # Not IPv4 format, just copy the value
    cassandra_addr_IPV6="$cassandra_addr"
  fi

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
  readonly take_a_snapshot
  readonly load_a_snapshot


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
