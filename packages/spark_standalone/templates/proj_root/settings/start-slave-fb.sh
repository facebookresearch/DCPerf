#!/usr/bin/env bash

#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License.  You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Starts a slave on the machine this script is executed on.
#
# Environment Variables
#
#   SPARK_WORKER_INSTANCES  The number of worker instances to run on this
#                           slave.  Default is 1.
#   SPARK_WORKER_PORT       The base port number for the first worker. If set,
#                           subsequent workers will increment this number.  If
#                           unset, Spark will find a valid port number, but
#                           with no guarantee of a predictable pattern.
#   SPARK_WORKER_WEBUI_PORT The base port for the web interface of the first
#                           worker.  Subsequent workers will increment this
#                           number.  Default is 8081.

if [ -z "${SPARK_HOME}" ]; then
  # shellcheck disable=SC2006,SC2155,SC2164
  export SPARK_HOME="$(cd "`dirname "$0"`"/..; pwd)"
fi

# NOTE: This exact class name is matched downstream by SparkSubmit.
# Any changes need to be reflected there.
CLASS="org.apache.spark.deploy.worker.Worker"

# shellcheck disable=SC2199
if [[ $# -lt 1 ]] || [[ "$@" = *--help ]] || [[ "$@" = *-h ]]; then
  echo "Usage: ./sbin/start-slave.sh <master> [options]"
  pattern="Usage:"
  pattern+="\|Using Spark's default log4j profile:"
  pattern+="\|Registered signal handlers for"

  "${SPARK_HOME}"/bin/spark-class $CLASS --help 2>&1 | grep -v "$pattern" 1>&2
  exit 1
fi

# shellcheck disable=SC1090
. "${SPARK_HOME}/sbin/spark-config.sh"

# shellcheck disable=SC1090
. "${SPARK_HOME}/bin/load-spark-env.sh"

# First argument should be the master; we need to store it aside because we may
# need to insert arguments between it and the other arguments
MASTER=$1
shift

# Determine desired worker port
if [ "$SPARK_WORKER_WEBUI_PORT" = "" ]; then
  SPARK_WORKER_WEBUI_PORT=8081
fi

# Start up the appropriate number of workers on this machine.
# quick local function to start a worker
function start_instance {
  WORKER_NUM=$1
  shift

  if [ "$SPARK_WORKER_PORT" = "" ]; then
    PORT_FLAG=
    PORT_NUM=
  else
    PORT_FLAG="--port"
    # shellcheck disable=SC2004
    PORT_NUM=$(( $SPARK_WORKER_PORT + $WORKER_NUM - 1 ))
  fi
  # shellcheck disable=SC2004
  WEBUI_PORT=$(( $SPARK_WORKER_WEBUI_PORT + $WORKER_NUM - 1 ))

  # shellcheck disable=SC2086
  "${SPARK_HOME}/sbin"/spark-daemon.sh start $CLASS $WORKER_NUM \
     --webui-port "$WEBUI_PORT" $PORT_FLAG $PORT_NUM $MASTER "$@"
}

if [ "$SPARK_WORKER_INSTANCES" = "" ]; then
  if [ "$SPARK_WORKER_INDEX" = "" ]; then
    start_instance 1 "$@"
  else
    # shellcheck disable=SC2004
    start_instance $(( 1 + $SPARK_WORKER_INDEX )) "$@"
  fi
else
  # shellcheck disable=SC2004
  for ((i=0; i<$SPARK_WORKER_INSTANCES; i++)); do
    # shellcheck disable=SC2004
    start_instance $(( 1 + $i )) "$@"
  done
fi
