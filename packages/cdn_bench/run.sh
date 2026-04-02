#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# CDN Benchmark run script using foss_revproxy.
# Usage:
#   ./run.sh -d 60 -r 1000 -c 4 -S 100 -p h2

set -Eeuo pipefail

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "${SCRIPT_DIR}/../../")"
BIN_DIR="${BENCHPRESS_ROOT}/packages/cdn_bench/binaries/"
LOG_FILE="${SCRIPT_DIR}/cdn_bench_run.log"

# Clear previous log
true > "$LOG_FILE"
# Redirect all output to both stdout and log file
exec > >(tee -a "$LOG_FILE") 2>&1

# Parser dispatch tag — must be first line of stdout
echo "CDN"

###############################################################################
# Default parameters
###############################################################################
DURATION=60
TARGET_RPS=1000
NUM_CONNECTIONS=4
STREAMS_PER_CONNECTION=100
PROTOCOL="h2"
CONTENT_PORT=8082
PROXY_PORT=8081

###############################################################################
# Parse arguments
###############################################################################
usage() {
    cat << EOF
Usage: $0 [options]

Options:
    -d <seconds>     Test duration in seconds (default: $DURATION)
    -r <rps>         Target requests per second (default: $TARGET_RPS)
    -c <connections> Number of concurrent connections (default: $NUM_CONNECTIONS)
    -S <streams>     Max concurrent streams per connection (default: $STREAMS_PER_CONNECTION)
    -p <protocol>    Protocol: h1 or h2 (default: $PROTOCOL)
    -h               Show this help
EOF
    exit 1
}

while getopts "d:r:c:S:p:h" opt; do
  case $opt in
    d) DURATION="$OPTARG" ;;
    r) TARGET_RPS="$OPTARG" ;;
    c) NUM_CONNECTIONS="$OPTARG" ;;
    S) STREAMS_PER_CONNECTION="$OPTARG" ;;
    p) PROTOCOL="$OPTARG" ;;
    h) usage ;;
    *) usage ;;
  esac
done

# Validate protocol
case "$PROTOCOL" in
  h1|h2) ;;
  *) echo "ERROR: Invalid protocol '$PROTOCOL'. Must be h1 or h2."; exit 1 ;;
esac

# Determine plaintext_proto flag for proxy and content server
if [ "$PROTOCOL" = "h2" ]; then
  PLAINTEXT_PROTO="h2"
else
  PLAINTEXT_PROTO=""
fi

###############################################################################
# Add bundled libraries to search path (from install_cdn_bench.sh Step 5)
###############################################################################
if [ -d "${BIN_DIR}/lib" ]; then
  export LD_LIBRARY_PATH="${BIN_DIR}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

###############################################################################
# Verify binaries exist and can execute
###############################################################################
for binary in traffic_client proxy_server content_server; do
  if [ ! -x "${BIN_DIR}/${binary}" ]; then
    echo "ERROR: ${binary} not found at ${BIN_DIR}/${binary}"
    echo "Run: ./benchpress -b ehw install cdn_bench"
    exit 1
  fi
done

###############################################################################
# Kill stale processes from previous runs
###############################################################################
for port in "${CONTENT_PORT}" "${PROXY_PORT}"; do
  STALE_PID=$(ss -tlnp 2>/dev/null | grep ":${port}\b" | grep -oP 'pid=\K[0-9]+' || true)
  if [ -n "$STALE_PID" ]; then
    echo "WARNING: Killing stale process (PID ${STALE_PID}) on port ${port}"
    kill "$STALE_PID" 2>/dev/null || true
    sleep 1
    if kill -0 "$STALE_PID" 2>/dev/null; then
      kill -9 "$STALE_PID" 2>/dev/null || true
      sleep 1
    fi
  fi
done

###############################################################################
# Process management
###############################################################################
CONTENT_PID=""
PROXY_PID=""

cleanup() {
  echo ""
  echo "Cleaning up processes..."
  if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
    kill "$PROXY_PID" 2>/dev/null || true
    wait "$PROXY_PID" 2>/dev/null || true
  fi
  if [ -n "$CONTENT_PID" ] && kill -0 "$CONTENT_PID" 2>/dev/null; then
    kill "$CONTENT_PID" 2>/dev/null || true
    wait "$CONTENT_PID" 2>/dev/null || true
  fi
}

trap cleanup EXIT ERR

###############################################################################
# Print header
###############################################################################
echo "====================================================================="
echo "CDN Benchmark Execution"
echo "====================================================================="
echo ""
echo "Configuration"
echo "  Protocol: ${PROTOCOL}"
echo "  Duration: ${DURATION}"
echo "  Target RPS: ${TARGET_RPS}"
echo "  Connections: ${NUM_CONNECTIONS}"
echo "  Streams Per Connection: ${STREAMS_PER_CONNECTION}"
echo "  Content Server Port: ${CONTENT_PORT}"
echo "  Proxy Server Port: ${PROXY_PORT}"
echo ""

###############################################################################
# Start content_server
###############################################################################
echo "Starting content_server on port ${CONTENT_PORT}..."
CONTENT_ARGS=(
  --port="${CONTENT_PORT}"
)
if [ -n "$PLAINTEXT_PROTO" ]; then
  CONTENT_ARGS+=(--plaintext_proto="${PLAINTEXT_PROTO}")
fi

"${BIN_DIR}/content_server" "${CONTENT_ARGS[@]}" \
  2>"${SCRIPT_DIR}/.content_stderr" &
CONTENT_PID=$!
echo "  content_server PID: ${CONTENT_PID}"

###############################################################################
# Start proxy_server
###############################################################################
echo "Starting proxy_server on port ${PROXY_PORT}..."
PROXY_ARGS=(
  --port="${PROXY_PORT}"
  --backend_servers="::1"
  --backend_ports="${CONTENT_PORT}"
  --metrics_summary
  --metrics_interval=0
)
if [ -n "$PLAINTEXT_PROTO" ]; then
  PROXY_ARGS+=(--plaintext_proto="${PLAINTEXT_PROTO}")
  PROXY_ARGS+=(--backend_h2)
fi

"${BIN_DIR}/proxy_server" "${PROXY_ARGS[@]}" \
  2>"${SCRIPT_DIR}/.proxy_stderr" &
PROXY_PID=$!
echo "  proxy_server PID: ${PROXY_PID}"

###############################################################################
# Wait for servers to be ready
###############################################################################
echo ""
echo "Waiting for servers to start..."
MAX_WAIT=30
for port in "${CONTENT_PORT}" "${PROXY_PORT}"; do
  if [ "$port" = "${CONTENT_PORT}" ]; then
    SERVER_PID="$CONTENT_PID"
    SERVER_NAME="content_server"
    SERVER_STDERR="${SCRIPT_DIR}/.content_stderr"
  else
    SERVER_PID="$PROXY_PID"
    SERVER_NAME="proxy_server"
    SERVER_STDERR="${SCRIPT_DIR}/.proxy_stderr"
  fi

  waited=0
  while ! ss -tlnp 2>/dev/null | grep -q ":${port}\b" ; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo "ERROR: ${SERVER_NAME} (PID ${SERVER_PID}) exited before listening on port ${port}"
      if [ -s "$SERVER_STDERR" ]; then
        echo "--- ${SERVER_NAME} stderr ---"
        cat "$SERVER_STDERR"
        echo "--- end stderr ---"
      fi
      exit 1
    fi
    sleep 1
    waited=$((waited + 1))
    if [ "$waited" -ge "$MAX_WAIT" ]; then
      echo "ERROR: ${SERVER_NAME} on port ${port} did not start within ${MAX_WAIT}s"
      if [ -s "$SERVER_STDERR" ]; then
        echo "--- ${SERVER_NAME} stderr ---"
        cat "$SERVER_STDERR"
        echo "--- end stderr ---"
      fi
      exit 1
    fi
  done
  echo "  Port ${port} is listening (waited ${waited}s)"
done
echo ""

###############################################################################
# Run traffic_client
###############################################################################
echo "Starting traffic_client..."
echo "  Target: ::1:${PROXY_PORT}"
echo "  Duration: ${DURATION}s, RPS: ${TARGET_RPS}, Connections: ${NUM_CONNECTIONS}"
echo ""

CLIENT_EXIT_CODE=0
"${BIN_DIR}/traffic_client" \
  --target_host="::1" \
  --target_port="${PROXY_PORT}" \
  --target_rps="${TARGET_RPS}" \
  --duration_sec="${DURATION}" \
  --num_connections="${NUM_CONNECTIONS}" \
  --streams_per_connection="${STREAMS_PER_CONNECTION}" \
  2>"${SCRIPT_DIR}/.client_stderr" || CLIENT_EXIT_CODE=$?

echo ""

###############################################################################
# Stop servers gracefully
###############################################################################
echo "Stopping servers..."
cleanup

# Clear PIDs so trap doesn't try to kill again
PROXY_PID=""
CONTENT_PID=""

###############################################################################
# Parse and output metrics from XLOG stderr
###############################################################################
echo ""
echo "Client Results"

# Parse traffic_client stderr for final statistics
CLIENT_STDERR="${SCRIPT_DIR}/.client_stderr"
if [ -f "$CLIENT_STDERR" ]; then
  REQUESTS_SENT=$(grep -oP 'Requests sent: \K[0-9]+' "$CLIENT_STDERR" | tail -1 || echo "0")
  RESPONSES_RECEIVED=$(grep -oP 'Responses received: \K[0-9]+' "$CLIENT_STDERR" | tail -1 || echo "0")
  CLIENT_ERRORS=$(grep -oP 'Errors: \K[0-9]+' "$CLIENT_STDERR" | tail -1 || echo "0")
  CLIENT_RESETS=$(grep -oP 'Resets: \K[0-9]+' "$CLIENT_STDERR" | tail -1 || echo "0")
  ELAPSED_MS=$(grep -oP 'Elapsed time: \K[0-9]+' "$CLIENT_STDERR" | tail -1 || echo "0")
  CLIENT_ACTUAL_RPS=$(grep -oP 'Actual RPS: \K[0-9.]+' "$CLIENT_STDERR" | tail -1 || echo "0")
else
  REQUESTS_SENT=0
  RESPONSES_RECEIVED=0
  CLIENT_ERRORS=0
  CLIENT_RESETS=0
  ELAPSED_MS=0
  CLIENT_ACTUAL_RPS=0
fi

echo "  Requests Sent: ${REQUESTS_SENT}"
echo "  Responses Received: ${RESPONSES_RECEIVED}"
echo "  Errors: ${CLIENT_ERRORS}"
echo "  Resets: ${CLIENT_RESETS}"
echo "  Elapsed Time ms: ${ELAPSED_MS}"
echo "  Actual RPS: ${CLIENT_ACTUAL_RPS}"

echo ""
echo "Proxy Results"

# Parse proxy_server stderr for final statistics
PROXY_STDERR="${SCRIPT_DIR}/.proxy_stderr"
if [ -f "$PROXY_STDERR" ]; then
  PROXY_REQUESTS_RECEIVED=$(grep -oP 'Requests Received: \K[0-9]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_REQUESTS_SUCCEEDED=$(grep -oP 'Requests Succeeded: \K[0-9]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_REQUESTS_FAILED=$(grep -oP 'Requests Failed: \K[0-9]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_SUCCESS_RATE=$(grep -oP 'Success Rate: \K[0-9.]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_ACTUAL_RPS=$(grep -oP 'Actual RPS: \K[0-9.]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_AVG_LATENCY=$(grep -oP 'Avg Total Latency: \K[0-9.]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_BACKEND_LATENCY=$(grep -oP 'Avg Backend Latency: \K[0-9.]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_RETRIES_ATTEMPTED=$(grep -oP 'Retries Attempted: \K[0-9]+' "$PROXY_STDERR" | tail -1 || echo "0")
  PROXY_RETRIES_SUCCEEDED=$(grep -oP 'Retries Succeeded: \K[0-9]+' "$PROXY_STDERR" | tail -1 || echo "0")
else
  PROXY_REQUESTS_RECEIVED=0
  PROXY_REQUESTS_SUCCEEDED=0
  PROXY_REQUESTS_FAILED=0
  PROXY_SUCCESS_RATE=0
  PROXY_ACTUAL_RPS=0
  PROXY_AVG_LATENCY=0
  PROXY_BACKEND_LATENCY=0
  PROXY_RETRIES_ATTEMPTED=0
  PROXY_RETRIES_SUCCEEDED=0
fi

echo "  Requests Received: ${PROXY_REQUESTS_RECEIVED}"
echo "  Requests Succeeded: ${PROXY_REQUESTS_SUCCEEDED}"
echo "  Requests Failed: ${PROXY_REQUESTS_FAILED}"
echo "  Success Rate: ${PROXY_SUCCESS_RATE}%"
echo "  Actual RPS: ${PROXY_ACTUAL_RPS}"
echo "  Avg Total Latency ms: ${PROXY_AVG_LATENCY}"
echo "  Avg Backend Latency ms: ${PROXY_BACKEND_LATENCY}"
echo "  Retries Attempted: ${PROXY_RETRIES_ATTEMPTED}"
echo "  Retries Succeeded: ${PROXY_RETRIES_SUCCEEDED}"

echo ""
echo "Benchmark Execution Complete"
echo "Exit Code: ${CLIENT_EXIT_CODE}"

exit "${CLIENT_EXIT_CODE}"
