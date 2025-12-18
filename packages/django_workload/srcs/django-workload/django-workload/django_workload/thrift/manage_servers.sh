#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Thrift Server Manager - Start/Stop multiple Thrift server instances
# Similar to Django worker management, spawns N servers and manages them

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"
NUM_SERVERS="${NUM_SERVERS:-8}"  # Default to 8 servers (can override)
START_PORT="${START_PORT:-9110}"  # Default start port (can override, avoids 9100 node_exporter)
PID_FILE="$SCRIPT_DIR/thrift_servers.pids"
LOG_DIR="$SCRIPT_DIR/logs"
HAPROXY_CFG_TEMPLATE="$SCRIPT_DIR/haproxy_thrift.cfg.template"
HAPROXY_CFG="$SCRIPT_DIR/haproxy_thrift.cfg"
HAPROXY_PID="$SCRIPT_DIR/haproxy_thrift.pid"
HAPROXY_FRONTEND_PORT="${HAPROXY_FRONTEND_PORT:-9090}"
HAPROXY_STATS_PORT="${HAPROXY_STATS_PORT:-9099}"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[ThriftManager]${NC} $1" >&2
}

log_warn() {
    echo -e "${YELLOW}[ThriftManager]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ThriftManager]${NC} $1" >&2
}

# Check if a port is available
check_port_available() {
    local port=$1
    # Use lsof to check if port is in use
    if ss -tan | grep -q ":${port}" >/dev/null 2>&1; then
        return 1  # Port is in use
    else
        return 0  # Port is available
    fi
}

# Find N available ports starting from START_PORT
find_available_ports() {
    local num_needed=$1
    local start_port=$2
    local -a available_ports=()
    local current_port=$start_port
    local max_attempts=$((num_needed * 3))  # Try up to 3x the needed ports
    local attempts=0

    log_info "Scanning for $num_needed available ports starting from $start_port..."

    while [ ${#available_ports[@]} -lt "$num_needed" ] && [ "$attempts" -lt "$max_attempts" ]; do
        if check_port_available "$current_port"; then
            available_ports+=("$current_port")
            log_info "  ✓ Port $current_port is available"
        else
            log_warn "  ⚠ Port $current_port is already in use, skipping..."
        fi
        current_port=$((current_port + 1))
        attempts=$((attempts + 1))
    done

    if [ ${#available_ports[@]} -lt "$num_needed" ]; then
        log_error "Could not find $num_needed available ports!"
        log_error "Found only ${#available_ports[@]} available ports: ${available_ports[*]}"
        log_error "Try increasing START_PORT or reducing NUM_SERVERS"
        return 1
    fi

    # Export the available ports as a space-separated string
    echo "${available_ports[*]}"
    return 0
}

# Start multiple Thrift server instances
start_servers() {
    log_info "Starting $NUM_SERVERS Thrift server instances..."
    log_info "Configuration: START_PORT=$START_PORT, NUM_SERVERS=$NUM_SERVERS"

    # Create log directory
    mkdir -p "$LOG_DIR"

    # Find available ports
    local ports_str
    if ! ports_str=$(find_available_ports "$NUM_SERVERS" "$START_PORT"); then
        log_error "Failed to find available ports. Aborting."
        return 1
    fi

    # Convert space-separated string to array
    local -a ports
    read -r -a ports <<< "$ports_str"

    # Clean up old PID file
    rm -f "$PID_FILE"

    log_info "Starting servers on ports: ${ports[*]}"

    # Start each server instance
    for i in "${!ports[@]}"; do
        PORT=${ports[$i]}
        LOG_FILE="$LOG_DIR/thrift_server_${PORT}.log"

        log_info "Starting Thrift server #$i on port $PORT..."

        # Start server in background
        THRIFT_PORT=$PORT nohup "$PYTHON_BIN" "$SCRIPT_DIR/thrift_server.py" \
            > "$LOG_FILE" 2>&1 &

        SERVER_PID=$!
        echo "$SERVER_PID:$PORT" >> "$PID_FILE"

        log_info "  → Server #$i started (PID: $SERVER_PID, Port: $PORT, Log: $LOG_FILE)"

        # Small delay to avoid race conditions
        # sleep 0.2
    done

    log_info "Waiting for servers to initialize..."
    sleep 2

    # Verify all servers are running
    verify_servers
}

# Stop all Thrift server instances
stop_servers() {
    if [ ! -f "$PID_FILE" ]; then
        log_warn "No PID file found - servers may not be running"
        return
    fi

    log_info "Stopping Thrift servers..."

    while IFS=: read -r pid port; do
        if [ -n "$pid" ]; then
            if kill -0 "$pid" 2>/dev/null; then
                log_info "Stopping server on port $port (PID: $pid)..."
                kill "$pid" 2>/dev/null || true
            else
                log_warn "Server PID $pid (port $port) not running"
            fi
        fi
    done < "$PID_FILE"

    # Wait for processes to terminate
    sleep 1

    # Force kill any remaining processes
    while IFS=: read -r pid port; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            log_warn "Force killing server PID $pid (port $port)..."
            kill -9 "$pid" 2>/dev/null || true
        fi
    done < "$PID_FILE"

    rm -f "$PID_FILE"
    log_info "All Thrift servers stopped"
}

# Verify servers are running
verify_servers() {
    log_info "Verifying server status..."

    RUNNING=0
    TOTAL=0

    if [ -f "$PID_FILE" ]; then
        while IFS=: read -r pid port; do
            if [ -n "$pid" ]; then
                TOTAL=$((TOTAL + 1))
                if kill -0 "$pid" 2>/dev/null; then
                    RUNNING=$((RUNNING + 1))
                    # Check if port is listening
                    if ss -tln | grep -q ":${port}" >/dev/null 2>&1; then
                        log_info "  ✓ Server on port $port (PID: $pid) - RUNNING"
                    else
                        log_warn "  ⚠ Server PID $pid running but port $port not listening yet"
                    fi
                else
                    log_error "  ✗ Server on port $port (PID: $pid) - NOT RUNNING"
                fi
            fi
        done < "$PID_FILE"
    fi

    if [ $RUNNING -eq $TOTAL ] && [ $RUNNING -gt 0 ]; then
        log_info "All $RUNNING servers are running successfully!"
    else
        log_error "Only $RUNNING/$TOTAL servers are running"
        return 1
    fi
}

# Show server status
status_servers() {
    if [ ! -f "$PID_FILE" ]; then
        log_warn "No PID file found - servers not started via this script"
        return 1
    fi

    log_info "Thrift Server Status:"
    echo "----------------------------------------"

    while IFS=: read -r pid port; do
        if [ -n "$pid" ]; then
            if kill -0 "$pid" 2>/dev/null; then
                # Get CPU and memory usage
                CPU=$(ps -p "$pid" -o %cpu= 2>/dev/null | tr -d ' ' || echo "N/A")
                MEM=$(ps -p "$pid" -o %mem= 2>/dev/null | tr -d ' ' || echo "N/A")
                echo -e "Port $port (PID: $pid): ${GREEN}RUNNING${NC} - CPU: ${CPU}%, MEM: ${MEM}%"
            else
                echo -e "Port $port (PID: $pid): ${RED}STOPPED${NC}"
            fi
        fi
    done < "$PID_FILE"

    echo "----------------------------------------"
}

# Generate HAProxy configuration based on running servers
generate_haproxy_config() {
    if [ ! -f "$PID_FILE" ]; then
        log_error "No PID file found - cannot generate HAProxy config"
        return 1
    fi

    if [ ! -f "$HAPROXY_CFG_TEMPLATE" ]; then
        log_error "HAProxy template not found: $HAPROXY_CFG_TEMPLATE"
        return 1
    fi

    log_info "Generating HAProxy configuration from template..."

    # Read all ports from PID file
    local -a ports=()
    while IFS=: read -r pid port; do
        if [ -n "$port" ]; then
            ports+=("$port")
        fi
    done < "$PID_FILE"

    if [ ${#ports[@]} -eq 0 ]; then
        log_error "No server ports found in PID file"
        return 1
    fi

    log_info "Configuring HAProxy for ${#ports[@]} backend servers on ports: ${ports[*]}"

    # Generate server definitions
    local server_defs=""
    for i in "${!ports[@]}"; do
        port=${ports[$i]}
        server_defs+="    server thrift$i 127.0.0.1:$port check inter 2000ms fall 2 rise 2"$'\n'
    done

    # Copy template and replace placeholder with server definitions
    cp "$HAPROXY_CFG_TEMPLATE" "$HAPROXY_CFG"

    # Create a temporary file with server definitions
    local temp_servers
    temp_servers=$(mktemp)
    echo "$server_defs" > "$temp_servers"

    # Use sed to replace the range of placeholder lines with server definitions
    # First delete the placeholder lines, then read and insert the server definitions
    sed -i -e '/# SERVER_DEFINITIONS_PLACEHOLDER/,/# Server definitions will be inserted here by manage_servers.sh/d' \
           -e '/option tcp-check/r '"$temp_servers" "$HAPROXY_CFG"

    rm -f "$temp_servers"

    log_info "HAProxy configuration generated: $HAPROXY_CFG"
    log_info "  Frontend: 0.0.0.0:$HAPROXY_FRONTEND_PORT"
    log_info "  Backend: ${#ports[@]} servers"
    log_info "  Stats: http://localhost:$HAPROXY_STATS_PORT"

    return 0
}

# Start HAProxy load balancer
start_haproxy() {
    # Check if HAProxy is already running
    if [ -f "$HAPROXY_PID" ] && kill -0 "$(cat "$HAPROXY_PID")" 2>/dev/null; then
        log_warn "HAProxy is already running (PID: $(cat "$HAPROXY_PID"))"
        return 0
    fi

    # Generate config from running servers
    generate_haproxy_config || return 1

    log_info "Starting HAProxy load balancer..."

    # Check if haproxy is installed
    if ! command -v haproxy &> /dev/null; then
        log_error "HAProxy is not installed. Install it with: sudo yum install haproxy"
        return 1
    fi

    # Start HAProxy
    if haproxy -f "$HAPROXY_CFG" -D -p "$HAPROXY_PID" 2>&1 | while read -r line; do log_info "$line"; done && [ -f "$HAPROXY_PID" ]; then
        log_info "✓ HAProxy started successfully (PID: $(cat "$HAPROXY_PID"))"
        log_info "✓ Frontend listening on: 0.0.0.0:$HAPROXY_FRONTEND_PORT"
        log_info "✓ Stats page: http://localhost:$HAPROXY_STATS_PORT"
        log_info "✓ Clients should connect to: localhost:$HAPROXY_FRONTEND_PORT"
        return 0
    else
        log_error "Failed to start HAProxy"
        return 1
    fi
}

# Stop HAProxy load balancer
stop_haproxy() {
    if [ ! -f "$HAPROXY_PID" ]; then
        log_warn "HAProxy PID file not found - may not be running"
        return 0
    fi

    local pid
    pid=$(cat "$HAPROXY_PID")
    if kill -0 "$pid" 2>/dev/null; then
        log_info "Stopping HAProxy (PID: $pid)..."
        kill "$pid" 2>/dev/null || true
        sleep 1

        # Force kill if still running
        if kill -0 "$pid" 2>/dev/null; then
            log_warn "Force killing HAProxy (PID: $pid)..."
            kill -9 "$pid" 2>/dev/null || true
        fi

        rm -f "$HAPROXY_PID"
        log_info "HAProxy stopped"
    else
        log_warn "HAProxy process (PID: $pid) not running"
        rm -f "$HAPROXY_PID"
    fi

    return 0
}

# Check HAProxy status
status_haproxy() {
    if [ -f "$HAPROXY_PID" ] && kill -0 "$(cat "$HAPROXY_PID")" 2>/dev/null; then
        local pid
        pid=$(cat "$HAPROXY_PID")
        local cpu
        cpu=$(ps -p "$pid" -o %cpu= 2>/dev/null | tr -d ' ' || echo "N/A")
        local mem
        mem=$(ps -p "$pid" -o %mem= 2>/dev/null | tr -d ' ' || echo "N/A")
        echo -e "HAProxy (PID: $pid): ${GREEN}RUNNING${NC} - CPU: ${cpu}%, MEM: ${mem}%"
        echo "  Frontend: http://localhost:$HAPROXY_FRONTEND_PORT"
        echo "  Stats: http://localhost:$HAPROXY_STATS_PORT"
        return 0
    else
        echo -e "HAProxy: ${RED}STOPPED${NC}"
        return 1
    fi
}

# Start servers and HAProxy together
start_all() {
    log_info "Starting complete Thrift server stack..."

    # Start Thrift servers
    start_servers || return 1

    # Start HAProxy
    start_haproxy || return 1

    log_info "✓ Complete stack is running!"
    log_info ""
    log_info "Summary:"
    log_info "  - $NUM_SERVERS Thrift servers running"
    log_info "  - HAProxy load balancing on port $HAPROXY_FRONTEND_PORT"
    log_info "  - Stats available at http://localhost:$HAPROXY_STATS_PORT"
}

# Stop servers and HAProxy together
stop_all() {
    log_info "Stopping complete Thrift server stack..."

    # Stop HAProxy first
    stop_haproxy

    # Then stop Thrift servers
    stop_servers

    log_info "✓ Complete stack stopped"
}

# Show complete status
status_all() {
    log_info "Complete Thrift Stack Status:"
    echo "========================================"
    echo ""

    # Show HAProxy status
    status_haproxy
    echo ""

    # Show server status
    status_servers
}

# Restart all servers
restart_all() {
    log_info "Restarting complete Thrift server stack..."
    stop_all
    sleep 2
    start_all
}

# Show usage
usage() {
    cat <<EOF
Usage: $0 {start|stop|restart|status} [--with-haproxy]

Manage multiple Thrift RPC server instances with optional HAProxy load balancing.

Commands:
  start          Start Thrift servers (and optionally HAProxy with --with-haproxy)
  stop           Stop Thrift servers (and optionally HAProxy with --with-haproxy)
  restart        Restart Thrift servers (and optionally HAProxy with --with-haproxy)
  status         Show status of servers (and optionally HAProxy with --with-haproxy)
  verify         Verify all servers are running correctly

Environment Variables:
  NUM_SERVERS            Number of server instances to start (default: 8)
  START_PORT             First port to try (default: 9110)
  HAPROXY_FRONTEND_PORT  HAProxy frontend port (default: 9090)
  HAPROXY_STATS_PORT     HAProxy stats page port (default: 9099)
  PYTHON_BIN             Python interpreter to use (default: python3)

Examples:
  $0 start --with-haproxy           # Start 8 Thrift servers + HAProxy
  NUM_SERVERS=16 $0 start           # Start 16 Thrift servers only
  START_PORT=9200 $0 start          # Start servers beginning at port 9200
  $0 status --with-haproxy          # Show status of servers and HAProxy
  $0 stop --with-haproxy            # Stop servers and HAProxy

EOF
}

# Main command dispatcher
WITH_HAPROXY=false
if [ "${2:-}" = "--with-haproxy" ]; then
    WITH_HAPROXY=true
fi

case "${1:-}" in
    start)
        if [ "$WITH_HAPROXY" = true ]; then
            start_all
        else
            start_servers
        fi
        ;;
    stop)
        if [ "$WITH_HAPROXY" = true ]; then
            stop_all
        else
            stop_servers
        fi
        ;;
    restart)
        if [ "$WITH_HAPROXY" = true ]; then
            restart_all
        else
            log_info "Restarting Thrift servers..."
            stop_servers
            sleep 2
            start_servers
        fi
        ;;
    status)
        if [ "$WITH_HAPROXY" = true ]; then
            status_all
        else
            status_servers
        fi
        ;;
    verify)
        verify_servers
        ;;
    *)
        usage
        exit 1
        ;;
esac
