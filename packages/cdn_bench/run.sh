#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# CDN Benchmark run script using foss_revproxy.
#
# Modes:
#   server           — run content_server(s) only
#   proxy            — run proxy_server(s) only
#   client           — run traffic_client(s) only

# Usage:
#   Server role:   ./run.sh -m server -P 8082,8083,8084
#   Proxy role:    ./run.sh -m proxy -B "server1,server1" -b "8082,8083" -P 8081
#   Client role:   ./run.sh -m client -T "proxy1:8081" -d 60 -r 1000

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
MODE="server"
DURATION=60
TARGET_RPS=1000
NUM_CONNECTIONS=4
STREAMS_PER_CONNECTION=100
PROTOCOL="h2"
CONTENT_PORT=8082
PROXY_PORT=8081

# Multi-host parameters
LISTEN_PORTS=""         # -P: comma-separated ports for server/proxy instances
BACKEND_HOSTS=""        # -B: comma-separated backend hosts for proxy
BACKEND_PORTS=""        # -b: comma-separated backend ports for proxy
PROXY_TARGETS=""        # -T: comma-separated host:port pairs for client targets

###############################################################################
# Parse arguments
###############################################################################
usage() {
    cat << EOF
Usage: $0 [options]

Modes:
    -m <mode>        Mode: server, proxy, client (default: server)

Multi-host options:
    -P <ports>       Comma-separated listen ports for server/proxy instances
    -B <backends>    Comma-separated backend hosts for proxy
    -b <ports>       Comma-separated backend ports for proxy
    -T <targets>     Comma-separated host:port pairs for client targets

    -h               Show this help
EOF
    exit 1
}

while getopts "m:d:r:c:S:p:P:B:b:T:h" opt; do
  case $opt in
    m) MODE="$OPTARG" ;;
    d) DURATION="$OPTARG" ;;
    r) TARGET_RPS="$OPTARG" ;;
    c) NUM_CONNECTIONS="$OPTARG" ;;
    S) STREAMS_PER_CONNECTION="$OPTARG" ;;
    p) PROTOCOL="$OPTARG" ;;
    P) LISTEN_PORTS="$OPTARG" ;;
    B) BACKEND_HOSTS="$OPTARG" ;;
    b) BACKEND_PORTS="$OPTARG" ;;
    T) PROXY_TARGETS="$OPTARG" ;;
    h) usage ;;
    *) usage ;;
  esac
done

# Validate mode
case "$MODE" in
  server|proxy|client) ;;
  *) echo "ERROR: Invalid mode '$MODE'. Must be server, proxy, or client."; exit 1 ;;
esac

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
# Verify binaries exist (only check binaries needed for this mode)
###############################################################################
check_binary() {
  local binary="$1"
  if [ ! -x "${BIN_DIR}/${binary}" ]; then
    echo "ERROR: ${binary} not found at ${BIN_DIR}/${binary}"
    echo "Run: ./benchpress -b ehw install cdn_bench"
    exit 1
  fi
}

case "$MODE" in
  server) check_binary content_server ;;
  proxy)  check_binary proxy_server ;;
  client) check_binary traffic_client ;;
esac

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
declare -a BG_PIDS=()

cleanup() {
  echo ""
  echo "Cleaning up processes..."
  for pid in "${BG_PIDS[@]}"; do
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  # Remove temp files matching our pattern
  rm -f "${SCRIPT_DIR}"/.content_stderr* "${SCRIPT_DIR}"/.proxy_stderr* "${SCRIPT_DIR}"/.client_stderr*
}

trap cleanup EXIT ERR

###############################################################################
# Helper: wait for port to be listening
###############################################################################
wait_for_port() {
  local port="$1"
  local max_wait="${2:-30}"
  local waited=0
  while ! ss -tlnp 2>/dev/null | grep -q ":${port} " ; do
    sleep 1
    waited=$((waited + 1))
    if [ "$waited" -ge "$max_wait" ]; then
      echo "ERROR: Server on port ${port} did not start within ${max_wait}s"
      exit 1
    fi
  done
  echo "  Port ${port} is listening (waited ${waited}s)"
}

###############################################################################
# Helper: start content_server instance
###############################################################################
start_content_server() {
  local port="$1"
  local stderr_file="$2"
  local args=(
    --port="${port}"
  )
  if [ -n "$PLAINTEXT_PROTO" ]; then
    args+=(--plaintext_proto="${PLAINTEXT_PROTO}")
  fi
  "${BIN_DIR}/content_server" "${args[@]}" 2>"${stderr_file}" &
  local pid=$!
  BG_PIDS+=("$pid")
  echo "  content_server PID: ${pid} (port ${port})"
}

###############################################################################
# Helper: verify content_server is serving requests (not just listening)
###############################################################################
verify_content_server() {
  local host="$1"
  local port="$2"
  echo -n "  Probing content_server at ${host}:${port} ... "
  local http_code
  http_code=$(curl -sf --connect-timeout 5 -o /dev/null -w "%{http_code}" "http://[${host}]:${port}/" 2>/dev/null || echo "000")
  if [ "$http_code" = "000" ]; then
    echo "-x> no response (connection refused or timeout)"
    return 1
  elif [ "$http_code" -ge 200 ] && [ "$http_code" -lt 400 ]; then
    echo "-> HTTP ${http_code}"
    return 0
  else
    echo "-x>  HTTP ${http_code} (server responded but with error)"
    return 0
  fi
}

###############################################################################
# Helper: start proxy_server instance
###############################################################################
start_proxy_server() {
  local port="$1"
  local backend_servers="$2"
  local backend_ports="$3"
  local stderr_file="$4"
  local args=(
    --port="${port}"
    --backend_servers="${backend_servers}"
    --backend_ports="${backend_ports}"
    --metrics_summary
    --metrics_interval=0
  )
  if [ -n "$PLAINTEXT_PROTO" ]; then
    args+=(--backend_h2)
  fi
  "${BIN_DIR}/proxy_server" "${args[@]}" 2>"${stderr_file}" &
  local pid=$!
  BG_PIDS+=("$pid")
  echo "  proxy_server PID: ${pid} (port ${port})"
}

###############################################################################
# Helper: run traffic_client instance
###############################################################################
run_traffic_client() {
  local target_host="$1"
  local target_port="$2"
  local stderr_file="$3"
  local exit_code=0
  "${BIN_DIR}/traffic_client" \
    --target_host="${target_host}" \
    --target_port="${target_port}" \
    --target_rps="${TARGET_RPS}" \
    --duration_sec="${DURATION}" \
    --num_connections="${NUM_CONNECTIONS}" \
    --streams_per_connection="${STREAMS_PER_CONNECTION}" \
    2>"${stderr_file}" || exit_code=$?
  return "$exit_code"
}

###############################################################################
# Helper: parse client stderr
###############################################################################
parse_client_stderr() {
  local stderr_file="$1"
  local prefix="${2:-}"
  if [ -f "$stderr_file" ]; then
    local requests_sent responses_received errors resets elapsed_ms actual_rps
    requests_sent=$(grep -oP 'Requests sent: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    responses_received=$(grep -oP 'Responses received: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    errors=$(grep -oP 'Errors: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    resets=$(grep -oP 'Resets: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    elapsed_ms=$(grep -oP 'Elapsed time: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    actual_rps=$(grep -oP 'Actual RPS: \K[0-9.]+' "$stderr_file" | tail -1 || echo "0")
    echo "  ${prefix}Requests Sent: ${requests_sent}"
    echo "  ${prefix}Responses Received: ${responses_received}"
    echo "  ${prefix}Errors: ${errors}"
    echo "  ${prefix}Resets: ${resets}"
    echo "  ${prefix}Elapsed Time ms: ${elapsed_ms}"
    echo "  ${prefix}Actual RPS: ${actual_rps}"
  fi
}

###############################################################################
# Helper: parse proxy stderr
###############################################################################
parse_proxy_stderr() {
  local stderr_file="$1"
  local prefix="${2:-}"
  if [ -f "$stderr_file" ]; then
    local req_recv req_succ req_fail success_rate actual_rps avg_latency backend_latency retries_attempted retries_succeeded
    req_recv=$(grep -oP 'Requests Received: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    req_succ=$(grep -oP 'Requests Succeeded: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    req_fail=$(grep -oP 'Requests Failed: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    success_rate=$(grep -oP 'Success Rate: \K[0-9.]+' "$stderr_file" | tail -1 || echo "0")
    actual_rps=$(grep -oP 'Actual RPS: \K[0-9.]+' "$stderr_file" | tail -1 || echo "0")
    avg_latency=$(grep -oP 'Avg Total Latency: \K[0-9.]+' "$stderr_file" | tail -1 || echo "0")
    backend_latency=$(grep -oP 'Avg Backend Latency: \K[0-9.]+' "$stderr_file" | tail -1 || echo "0")
    retries_attempted=$(grep -oP 'Retries Attempted: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    retries_succeeded=$(grep -oP 'Retries Succeeded: \K[0-9]+' "$stderr_file" | tail -1 || echo "0")
    echo "  ${prefix}Requests Received: ${req_recv}"
    echo "  ${prefix}Requests Succeeded: ${req_succ}"
    echo "  ${prefix}Requests Failed: ${req_fail}"
    echo "  ${prefix}Success Rate: ${success_rate}%"
    echo "  ${prefix}Actual RPS: ${actual_rps}"
    echo "  ${prefix}Avg Total Latency ms: ${avg_latency}"
    echo "  ${prefix}Avg Backend Latency ms: ${backend_latency}"
    echo "  ${prefix}Retries Attempted: ${retries_attempted}"
    echo "  ${prefix}Retries Succeeded: ${retries_succeeded}"
  fi
}


###############################################################################
# MODE: server — run content_server instance(s)
###############################################################################
run_server() {
  local ports="${LISTEN_PORTS:-${CONTENT_PORT}}"

  echo "====================================================================="
  echo "CDN Benchmark — Server Role"
  echo "====================================================================="
  echo ""
  echo "Configuration"
  echo "  Mode: server"
  echo "  Protocol: ${PROTOCOL}"
  echo "  Ports: ${ports}"
  echo ""

  IFS=',' read -ra PORT_ARRAY <<< "$ports"
  local idx=0
  for port in "${PORT_ARRAY[@]}"; do
    port="$(echo "$port" | tr -d ' ')"
    echo "Starting content_server instance ${idx} on port ${port}..."
    start_content_server "${port}" "${SCRIPT_DIR}/.content_stderr_${idx}"
    idx=$((idx + 1))
  done

  echo ""
  echo "Waiting for servers to start..."
  for port in "${PORT_ARRAY[@]}"; do
    port="$(echo "$port" | tr -d ' ')"
    wait_for_port "${port}"
  done

  # Verify content_servers are actually serving responses
  echo ""
  echo "====================================================================="
  echo "Content Server Health Check"
  echo "====================================================================="
  local hostname_ip
  hostname_ip=$(hostname -I | awk '{print $1}')
  for port in "${PORT_ARRAY[@]}"; do
    port="$(echo "$port" | tr -d ' ')"
    verify_content_server "::1" "${port}" || verify_content_server "${hostname_ip}" "${port}" || true
  done
  echo ""

  echo ""
  echo "All content_server instances running. Waiting for termination signal..."
  echo "Send SIGTERM or SIGINT to stop."

  # Wait for any background process to exit (or signal)
  wait
}

###############################################################################
# MODE: proxy — run proxy_server instance(s)
###############################################################################
run_proxy() {
  local ports="${LISTEN_PORTS:-${PROXY_PORT}}"

  if [ -z "$BACKEND_HOSTS" ]; then
    echo "ERROR: Proxy mode requires -B <backend_hosts> (comma-separated backend hosts)"
    exit 1
  fi
  if [ -z "$BACKEND_PORTS" ]; then
    echo "ERROR: Proxy mode requires -b <backend_ports> (comma-separated backend ports)"
    exit 1
  fi

  local backend_servers="$BACKEND_HOSTS"
  local backend_ports="$BACKEND_PORTS"

  echo "====================================================================="
  echo "CDN Benchmark — Proxy Role"
  echo "====================================================================="
  echo ""
  echo "Configuration"
  echo "  Mode: proxy"
  echo "  Protocol: ${PROTOCOL}"
  echo "  Listen Ports: ${ports}"
  echo "  Backend Servers: ${backend_servers}"
  echo "  Backend Ports: ${backend_ports}"
  echo ""

  # =========================================================================
  # Sanity check: verify all backends are reachable before starting proxies
  # =========================================================================
  echo "====================================================================="
  echo "Backend Reachability Check"
  echo "====================================================================="
  IFS=',' read -ra BHOST_ARRAY <<< "$backend_servers"
  IFS=',' read -ra BPORT_ARRAY <<< "$backend_ports"
  local all_backends_ok=true
  local checked=()

  for i in "${!BHOST_ARRAY[@]}"; do
    local bhost="${BHOST_ARRAY[$i]}"
    local bport="${BPORT_ARRAY[$i]:-${BPORT_ARRAY[0]}}"
    bhost="$(echo "$bhost" | tr -d ' ')"
    bport="$(echo "$bport" | tr -d ' ')"
    local key="${bhost}:${bport}"

    # Skip duplicates (same host:port may appear multiple times for load balancing)
    local already_checked=false
    for c in "${checked[@]:-}"; do
      [ "$c" = "$key" ] && already_checked=true && break
    done
    "$already_checked" && continue
    checked+=("$key")

    echo -n "  Checking backend ${bhost}:${bport} ... "
    if curl -sf --connect-timeout 5 -o /dev/null "http://[${bhost}]:${bport}/" 2>/dev/null; then
      echo "-> reachable"
    else
      echo "-x-> UNREACHABLE"
      all_backends_ok=false
    fi
  done

  echo ""
  if [ "$all_backends_ok" = false ]; then
    echo "====================================================================="
    echo "ERROR: One or more backends are not reachable!"
    echo "====================================================================="
    echo ""
    echo "  Ensure content_server is running on the backend host(s):"
    echo "    ./benchpress -b ehw run cdn_bench --role server -i 1 \\"
    echo "      --role_input='{\"ports\":\"${backend_ports}\",\"protocol\":\"${PROTOCOL}\"}'"
    echo ""
    echo "  Then verify from this host:"
    for c in "${checked[@]}"; do
      local h="${c%:*}"
      local p="${c##*:}"
      echo "    curl -sf http://[${h}]:${p}/"
    done
    echo ""
    echo "Aborting proxy startup."
    exit 1
  fi
  echo "All backends reachable"
  echo ""

  IFS=',' read -ra PORT_ARRAY <<< "$ports"
  local idx=0
  for port in "${PORT_ARRAY[@]}"; do
    port="$(echo "$port" | tr -d ' ')"
    echo "Starting proxy_server instance ${idx} on port ${port}..."
    start_proxy_server "${port}" "${backend_servers}" "${backend_ports}" \
      "${SCRIPT_DIR}/.proxy_stderr_${idx}"
    idx=$((idx + 1))
  done

  echo ""
  echo "Waiting for proxy servers to start..."
  for port in "${PORT_ARRAY[@]}"; do
    port="$(echo "$port" | tr -d ' ')"
    wait_for_port "${port}"
  done

  echo ""
  echo "All proxy_server instances running. Waiting for termination signal..."
  echo "Send SIGTERM or SIGINT to stop."

  # Wait for any background process to exit (or signal)
  wait || true

  # Output proxy metrics for all instances
  echo ""
  idx=0
  for port in "${PORT_ARRAY[@]}"; do
    echo "Proxy Results (instance ${idx}, port $(echo "$port" | tr -d ' '))"
    parse_proxy_stderr "${SCRIPT_DIR}/.proxy_stderr_${idx}"
    echo ""
    idx=$((idx + 1))
  done

  echo "Proxy Role Complete"
}

###############################################################################
# MODE: client — run traffic_client instance(s)
###############################################################################
run_client() {
  if [ -z "$PROXY_TARGETS" ]; then
    echo "ERROR: Client mode requires -T <proxy_targets> (comma-separated host:port pairs)"
    exit 1
  fi

  echo "====================================================================="
  echo "CDN Benchmark — Client Role"
  echo "====================================================================="
  echo ""
  echo "Configuration"
  echo "  Mode: client"
  echo "  Protocol: ${PROTOCOL}"
  echo "  Duration: ${DURATION}"
  echo "  Target RPS: ${TARGET_RPS}"
  echo "  Connections: ${NUM_CONNECTIONS}"
  echo "  Streams Per Connection: ${STREAMS_PER_CONNECTION}"
  echo "  Proxy Targets: ${PROXY_TARGETS}"
  echo ""

  IFS=',' read -ra TARGET_ARRAY <<< "$PROXY_TARGETS"
  local num_targets=${#TARGET_ARRAY[@]}

  if [ "$num_targets" -eq 1 ]; then
    # Single target: run in foreground
    local entry="${TARGET_ARRAY[0]}"
    entry="$(echo "$entry" | tr -d ' ')"
    # IPv6-safe host:port parsing — port is the last colon-separated field if numeric
    local port="${entry##*:}"
    local host="${entry%:$port}"

    echo "Starting traffic_client..."
    echo "  Target: ${host}:${port}"
    echo "  Duration: ${DURATION}s, RPS: ${TARGET_RPS}, Connections: ${NUM_CONNECTIONS}"
    echo ""

    CLIENT_EXIT_CODE=0
    run_traffic_client "${host}" "${port}" "${SCRIPT_DIR}/.client_stderr_0" || CLIENT_EXIT_CODE=$?

    echo ""
    echo "Client Results"
    parse_client_stderr "${SCRIPT_DIR}/.client_stderr_0"
  else
    # Multiple targets: run each in background, wait for all
    local idx=0
    declare -a CLIENT_PIDS=()
    for entry in "${TARGET_ARRAY[@]}"; do
      entry="$(echo "$entry" | tr -d ' ')"
      # IPv6-safe host:port parsing
      local port="${entry##*:}"
      local host="${entry%:$port}"

      echo "Starting traffic_client instance ${idx} targeting ${host}:${port}..."
      run_traffic_client "${host}" "${port}" "${SCRIPT_DIR}/.client_stderr_${idx}" &
      CLIENT_PIDS+=($!)
      idx=$((idx + 1))
    done

    echo ""
    echo "Waiting for ${num_targets} client instances to complete..."

    CLIENT_EXIT_CODE=0
    for pid in "${CLIENT_PIDS[@]}"; do
      wait "$pid" || CLIENT_EXIT_CODE=$?
    done

    echo ""
    idx=0
    for entry in "${TARGET_ARRAY[@]}"; do
      entry="$(echo "$entry" | tr -d ' ')"
      echo "Client Results (instance ${idx}, target ${entry})"
      parse_client_stderr "${SCRIPT_DIR}/.client_stderr_${idx}"
      echo ""
      idx=$((idx + 1))
    done
  fi

  echo ""
  echo "Benchmark Execution Complete"
  echo "Exit Code: ${CLIENT_EXIT_CODE}"

  rm -f "${SCRIPT_DIR}"/.client_stderr*
  return "$CLIENT_EXIT_CODE"
}

###############################################################################
# Dispatch to mode
###############################################################################
case "$MODE" in
  server) run_server ;;
  proxy)  run_proxy ;;
  client) run_client ;;
esac
