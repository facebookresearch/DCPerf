#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -Eeuo pipefail

# Get the directory where this script is located
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
LOG_FILE="${SCRIPT_DIR}/nic_run.log"

# Clear previous log file
true > "$LOG_FILE"

# Redirect all output to both stdout and log file
exec > >(tee -a "$LOG_FILE") 2>&1

echo "NIC"

###############################################################################
# iperf3 Benchmark Run Script
#
# Purpose:
#   Run iperf3 benchmark with comprehensive NIC and system metadata collection.
#   Designed for dc perf integration - vmstat and perf metrics are collected
#   separately by the dc perf framework.
#
# Example Usage:
#   Server: ./run.sh -m server -p 5201
#   Client: ./run.sh -m client -s 10.0.0.1 -p 5201 -t 60 -P 4
#   UDP:    ./run.sh -m client -s 10.0.0.1 -u -b 10G
#   NUMA:   ./run.sh -m client -s 10.0.0.1 -N 0 -M 0 -P 32
#
#   Multiple servers (parallel instances):
#     ./run.sh -m client -s "10.0.0.1,10.0.0.2" -p "5201,5202" -N 0 -M 0 -P 32
#
###############################################################################

usage() {
    cat << EOF
Usage: $0 [options]

Options:
    -m <mode>            Mode: server or client (default: client)
    -s <server_ip>       Server IP address(es), comma-separated for multiple
    -p <port>            Port number(s), comma-separated for multiple (default: 5201)
    -t <duration>        Test duration in seconds (default: 60)
    -P <parallel>        Number of parallel streams per instance (default: 1)
    -i <interval>        Reporting interval in seconds (default: 1)
    -n <nic_interface>   NIC interface to monitor (default: auto-detect)
    -b <bandwidth>       Target bandwidth, e.g., 10G, 1M (default: unlimited)
    -R                   Reverse mode (server sends, client receives)
    -u                   Use UDP instead of TCP
    -l <length>          Buffer length (default: 128K TCP, 8K UDP)
    -w <window>          Socket buffer size / TCP window size
    -Z                   Use zero-copy method
    -A <affinity>        CPU affinity, e.g., "0,1" or "0-3"
    -V                   Verbose output (more detailed iperf3 output)
    -T <title>           Title prefix for iperf3 output lines
    -f <format>          Output format: k/m/g/t (Kbits/Mbits/Gbits/Tbits)
    -N <numa_cpu>        NUMA CPU node binding (numactl --cpunodebind)
    -M <numa_mem>        NUMA memory node binding (numactl --membind)
    -h                   Show this help message

Note: Server mode always runs in one-off mode (-1) to exit after handling client connections.

Examples:
    Server: $0 -m server -p 5201
    Client: $0 -m client -s 10.0.0.1 -p 5201 -t 60 -P 4
    UDP:    $0 -m client -s 10.0.0.1 -u -b 10G -t 30
    NUMA:   $0 -m client -s 10.0.0.1 -N 0 -M 0 -P 32

    Multiple parallel instances to different servers:
      $0 -m client -s "10.0.0.1,10.0.0.2" -p "5201,5202" -N 0 -M 0 -P 32

EOF
    exit 1
}

# Default values
MODE="client"
SERVER_IP=""
PORT="5201"
DURATION="60"
PARALLEL="1"
INTERVAL="1"
NIC_INTERFACE=""
BANDWIDTH=""
REVERSE=""
UDP=""
BUFFER_LENGTH=""
WINDOW_SIZE=""
ZERO_COPY=""
CPU_AFFINITY=""
VERBOSE=""
TITLE=""
FORMAT=""
NUMA_CPU=""
NUMA_MEM=""

# Parse arguments
while getopts "m:s:p:t:P:i:n:b:Rul:w:ZA:VT:f:N:M:h" opt; do
    case "$opt" in
        m) MODE="$OPTARG" ;;
        s) SERVER_IP="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) DURATION="$OPTARG" ;;
        P) PARALLEL="$OPTARG" ;;
        i) INTERVAL="$OPTARG" ;;
        n) NIC_INTERFACE="$OPTARG" ;;
        b) BANDWIDTH="$OPTARG" ;;
        R) REVERSE="-R" ;;
        u) UDP="-u" ;;
        l) BUFFER_LENGTH="$OPTARG" ;;
        w) WINDOW_SIZE="$OPTARG" ;;
        Z) ZERO_COPY="-Z" ;;
        A) CPU_AFFINITY="$OPTARG" ;;
        V) VERBOSE="-V" ;;
        T) TITLE="$OPTARG" ;;
        f) FORMAT="$OPTARG" ;;
        N) NUMA_CPU="$OPTARG" ;;
        M) NUMA_MEM="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

# Validate mode
if [[ "$MODE" != "server" && "$MODE" != "client" ]]; then
    echo "ERROR: Mode must be 'server' or 'client'"
    usage
fi

# Validate client mode requires server IP
if [[ "$MODE" == "client" && -z "$SERVER_IP" ]]; then
    echo "ERROR: Client mode requires server IP (-s)"
    usage
fi

# Check if iperf3 is installed
if ! command -v iperf3 &> /dev/null; then
    echo "ERROR: iperf3 not found. Please run install_nic_micro.sh first."
    exit 1
fi

# Auto-detect NIC interface if not specified
if [[ -z "$NIC_INTERFACE" ]]; then
    # Try to find the default route interface
    NIC_INTERFACE=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'dev \K\S+' | head -1) || true
    if [[ -z "$NIC_INTERFACE" ]]; then
        # Fallback: find first non-loopback interface
        NIC_INTERFACE=$(ip -o link show | awk -F': ' '$2 != "lo" {print $2; exit}' | cut -d'@' -f1) || true
    fi
fi

# Function to get NIC information
get_nic_info() {
    local iface="$1"
    echo "====================================================================="
    echo "NIC Information: $iface"
    echo "====================================================================="

    if [[ -z "$iface" || ! -d "/sys/class/net/$iface" ]]; then
        echo "WARNING: NIC interface '$iface' not found or invalid"
        return
    fi

    # Basic interface info
    echo "Interface: $iface"
    echo "MAC Address: $(cat /sys/class/net/"$iface"/address 2>/dev/null || echo 'N/A')"
    echo "MTU: $(cat /sys/class/net/"$iface"/mtu 2>/dev/null || echo 'N/A')"
    echo "Operstate: $(cat /sys/class/net/"$iface"/operstate 2>/dev/null || echo 'N/A')"
    echo "Speed: $(cat /sys/class/net/"$iface"/speed 2>/dev/null || echo 'N/A') Mbps"
    echo "Duplex: $(cat /sys/class/net/"$iface"/duplex 2>/dev/null || echo 'N/A')"

    # Ring buffer sizes
    echo ""
    echo "Ring Buffer Configuration:"
    if command -v ethtool &> /dev/null; then
        ethtool -g "$iface" 2>/dev/null | head -20 || echo "  Ring buffer info not available"
    fi

    # Queue count
    echo ""
    echo "Queue Configuration:"
    if command -v ethtool &> /dev/null; then
        ethtool -l "$iface" 2>/dev/null | head -15 || echo "  Queue info not available"
    fi

    # Offload settings
    echo ""
    echo "Offload Features:"
    if command -v ethtool &> /dev/null; then
        ethtool -k "$iface" 2>/dev/null | grep -E 'tx-checksum|rx-checksum|tso|gso|gro|lro' || echo "  Offload info not available"
    fi

    # IRQ affinity summary
    echo ""
    echo "IRQ Affinity Summary:"
    local irq_count=0
    if [[ -d "/sys/class/net/$iface/device/msi_irqs" ]]; then
        irq_count=$(ls /sys/class/net/"$iface"/device/msi_irqs 2>/dev/null | wc -l)
        echo "  MSI-X IRQs: $irq_count"
    else
        echo "  MSI-X info not available"
    fi

    # IP addresses
    echo ""
    echo "IP Addresses:"
    ip addr show "$iface" 2>/dev/null | grep -E 'inet |inet6 ' | awk '{print "  " $1 " " $2}' || echo "  No IP addresses assigned"

    echo ""
}

# Function to get system network configuration
get_network_sysctl() {
    echo "====================================================================="
    echo "Network Kernel Parameters"
    echo "====================================================================="
    echo "TCP Buffer Sizes:"
    echo "  net.core.rmem_max: $(sysctl -n net.core.rmem_max 2>/dev/null || echo 'N/A')"
    echo "  net.core.wmem_max: $(sysctl -n net.core.wmem_max 2>/dev/null || echo 'N/A')"
    echo "  net.core.rmem_default: $(sysctl -n net.core.rmem_default 2>/dev/null || echo 'N/A')"
    echo "  net.core.wmem_default: $(sysctl -n net.core.wmem_default 2>/dev/null || echo 'N/A')"
    echo "  net.ipv4.tcp_rmem: $(sysctl -n net.ipv4.tcp_rmem 2>/dev/null || echo 'N/A')"
    echo "  net.ipv4.tcp_wmem: $(sysctl -n net.ipv4.tcp_wmem 2>/dev/null || echo 'N/A')"
    echo ""
    echo "Network Queue Settings:"
    echo "  net.core.netdev_max_backlog: $(sysctl -n net.core.netdev_max_backlog 2>/dev/null || echo 'N/A')"
    echo "  net.core.somaxconn: $(sysctl -n net.core.somaxconn 2>/dev/null || echo 'N/A')"
    echo ""
    echo "TCP Congestion Control:"
    echo "  net.ipv4.tcp_congestion_control: $(sysctl -n net.ipv4.tcp_congestion_control 2>/dev/null || echo 'N/A')"
    echo "  net.ipv4.tcp_available_congestion_control: $(sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null || echo 'N/A')"
    echo ""
}

# Build numactl prefix if NUMA binding is requested
build_numactl_prefix() {
    local prefix=""
    if [[ -n "$NUMA_CPU" || -n "$NUMA_MEM" ]]; then
        if ! command -v numactl &> /dev/null; then
            echo "ERROR: numactl not found but NUMA binding requested" >&2
            exit 1
        fi
        prefix="numactl"
        [[ -n "$NUMA_CPU" ]] && prefix="$prefix --cpunodebind=$NUMA_CPU"
        [[ -n "$NUMA_MEM" ]] && prefix="$prefix --membind=$NUMA_MEM"
    fi
    echo "$prefix"
}

# Build iperf3 command for a single instance
# $1: server IP (empty for server mode)
# $2: port number
# $3: title prefix
# $4: CPU affinity for this specific instance (optional)
build_single_iperf_cmd() {
    local server="$1"
    local port="$2"
    local title="$3"
    local instance_affinity="$4"
    local cmd="iperf3"

    if [[ "$MODE" == "server" ]]; then
        cmd="$cmd -s"
        cmd="$cmd -p $port"
        cmd="$cmd -i $INTERVAL"
        cmd="$cmd -1"  # Always run in one-off mode for dc perf metrics collection
        [[ -n "$VERBOSE" ]] && cmd="$cmd $VERBOSE"
        # Apply per-instance CPU affinity if provided
        [[ -n "$instance_affinity" ]] && cmd="$cmd -A $instance_affinity"
    else
        cmd="$cmd -c $server"
        cmd="$cmd -p $port"
        cmd="$cmd -t $DURATION"
        cmd="$cmd -P $PARALLEL"
        cmd="$cmd -i $INTERVAL"

        [[ -n "$BANDWIDTH" ]] && cmd="$cmd -b $BANDWIDTH"
        [[ -n "$REVERSE" ]] && cmd="$cmd $REVERSE"
        [[ -n "$UDP" ]] && cmd="$cmd $UDP"
        [[ -n "$BUFFER_LENGTH" ]] && cmd="$cmd -l $BUFFER_LENGTH"
        [[ -n "$WINDOW_SIZE" ]] && cmd="$cmd -w $WINDOW_SIZE"
        [[ -n "$ZERO_COPY" ]] && cmd="$cmd $ZERO_COPY"
        # Apply per-instance CPU affinity if provided, otherwise use global
        if [[ -n "$instance_affinity" ]]; then
            cmd="$cmd -A $instance_affinity"
        elif [[ -n "$CPU_AFFINITY" ]]; then
            cmd="$cmd -A $CPU_AFFINITY"
        fi
        [[ -n "$VERBOSE" ]] && cmd="$cmd $VERBOSE"
        [[ -n "$title" ]] && cmd="$cmd -T $title"
        [[ -n "$FORMAT" ]] && cmd="$cmd -f $FORMAT"
    fi

    echo "$cmd"
}

# Build full command with numactl prefix and multiple instances if needed
build_full_command() {
    local numactl_prefix
    numactl_prefix=$(build_numactl_prefix)

    # Parse comma-separated servers, ports, and CPU affinities
    IFS=',' read -ra SERVERS <<< "$SERVER_IP"
    IFS=',' read -ra PORTS <<< "$PORT"
    IFS=',' read -ra CPU_AFFINITIES <<< "$CPU_AFFINITY"

    local num_servers=${#SERVERS[@]}
    local num_ports=${#PORTS[@]}
    local num_affinities=${#CPU_AFFINITIES[@]}

    # If only one port specified, use it for all servers
    if [[ $num_ports -eq 1 && $num_servers -gt 1 ]]; then
        local single_port="${PORTS[0]}"
        PORTS=()
        for ((i=0; i<num_servers; i++)); do
            PORTS+=("$single_port")
        done
        num_ports=$num_servers
    fi

    # Server mode: may need multiple listeners
    if [[ "$MODE" == "server" ]]; then
        if [[ $num_ports -eq 1 ]]; then
            local instance_affinity=""
            [[ $num_affinities -ge 1 ]] && instance_affinity="${CPU_AFFINITIES[0]}"
            local cmd
            cmd=$(build_single_iperf_cmd "" "${PORTS[0]}" "" "$instance_affinity")
            [[ -n "$numactl_prefix" ]] && cmd="$numactl_prefix $cmd"
            echo "$cmd"
        else
            local full_cmd=""
            for ((i=0; i<num_ports; i++)); do
                # Get per-instance CPU affinity if available
                local instance_affinity=""
                [[ $i -lt $num_affinities ]] && instance_affinity="${CPU_AFFINITIES[$i]}"
                local cmd
                cmd=$(build_single_iperf_cmd "" "${PORTS[$i]}" "" "$instance_affinity")
                [[ -n "$numactl_prefix" ]] && cmd="$numactl_prefix $cmd"
                if [[ $i -lt $((num_ports - 1)) ]]; then
                    full_cmd="$full_cmd$cmd & "
                else
                    full_cmd="$full_cmd$cmd"
                fi
            done
            echo "$full_cmd"
        fi
        return
    fi

    # Client mode: may connect to multiple servers
    if [[ $num_servers -eq 1 ]]; then
        local instance_affinity=""
        [[ $num_affinities -ge 1 ]] && instance_affinity="${CPU_AFFINITIES[0]}"
        local cmd
        cmd=$(build_single_iperf_cmd "${SERVERS[0]}" "${PORTS[0]}" "$TITLE" "$instance_affinity")
        [[ -n "$numactl_prefix" ]] && cmd="$numactl_prefix $cmd"
        echo "$cmd"
    else
        local full_cmd=""
        for ((i=0; i<num_servers; i++)); do
            local port="${PORTS[$i]:-${PORTS[0]}}"
            local title_suffix=""
            [[ -n "$TITLE" ]] && title_suffix="${TITLE}${i}" || title_suffix="s$((i+1))"
            # Get per-instance CPU affinity if available
            local instance_affinity=""
            [[ $i -lt $num_affinities ]] && instance_affinity="${CPU_AFFINITIES[$i]}"
            local cmd
            cmd=$(build_single_iperf_cmd "${SERVERS[$i]}" "$port" "$title_suffix" "$instance_affinity")
            [[ -n "$numactl_prefix" ]] && cmd="$numactl_prefix $cmd"
            if [[ $i -lt $((num_servers - 1)) ]]; then
                full_cmd="$full_cmd$cmd & "
            else
                full_cmd="$full_cmd$cmd"
            fi
        done
        echo "$full_cmd"
    fi
}

# Legacy function for compatibility
build_iperf_cmd() {
    build_full_command
}

###############################################################################
# Main Execution
# All output goes to stdout - log persistence handled by copymove hook
###############################################################################

echo "NIC"
echo "====================================================================="
echo "iperf3 Benchmark Execution"
echo "====================================================================="
echo "Script: $0"
echo "Execution Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Hostname: $(hostname)"
echo "Kernel: $(uname -r)"
echo ""

# Log all arguments
echo "====================================================================="
echo "Arguments Passed"
echo "====================================================================="
echo "  Mode:              $MODE"
if [[ "$MODE" == "client" ]]; then
    echo "  Server IP:         $SERVER_IP"
fi
echo "  Port:              $PORT"
echo "  Duration:          $DURATION seconds"
echo "  Parallel Streams:  $PARALLEL"
echo "  Report Interval:   $INTERVAL seconds"
echo "  NIC Interface:     ${NIC_INTERFACE:-auto-detect}"
echo "  Bandwidth:         ${BANDWIDTH:-unlimited}"
echo "  Reverse Mode:      ${REVERSE:-no}"
echo "  UDP Mode:          ${UDP:-no (TCP)}"
echo "  Buffer Length:     ${BUFFER_LENGTH:-default}"
echo "  Window Size:       ${WINDOW_SIZE:-default}"
echo "  Zero-Copy:         ${ZERO_COPY:-no}"
echo "  CPU Affinity:      ${CPU_AFFINITY:-none}"
echo "  Verbose:           ${VERBOSE:-no}"
echo "  Title:             ${TITLE:-none}"
echo "  Output Format:     ${FORMAT:-default}"
echo "  NUMA CPU Bind:     ${NUMA_CPU:-none}"
echo "  NUMA Mem Bind:     ${NUMA_MEM:-none}"
echo ""

# Get NIC information
get_nic_info "$NIC_INTERFACE"

# Get network sysctl settings
get_network_sysctl

# Build and display the command
IPERF_CMD=$(build_iperf_cmd)
echo "====================================================================="
echo "Running iperf3 Benchmark"
echo "====================================================================="
echo "Command: $IPERF_CMD"
echo ""

# Run iperf3
echo "====================================================================="
echo "iperf3 Output"
echo "====================================================================="
eval "$IPERF_CMD" 2>&1
IPERF_EXIT_CODE=$?
echo ""

# Post-benchmark NIC statistics
echo "====================================================================="
echo "Post-Benchmark NIC Statistics"
echo "====================================================================="
if [[ -n "$NIC_INTERFACE" && -d "/sys/class/net/$NIC_INTERFACE" ]]; then
    echo "Interface Statistics ($NIC_INTERFACE):"
    echo "  RX bytes:   $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/rx_bytes 2>/dev/null || echo 'N/A')"
    echo "  TX bytes:   $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/tx_bytes 2>/dev/null || echo 'N/A')"
    echo "  RX packets: $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/rx_packets 2>/dev/null || echo 'N/A')"
    echo "  TX packets: $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/tx_packets 2>/dev/null || echo 'N/A')"
    echo "  RX errors:  $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/rx_errors 2>/dev/null || echo 'N/A')"
    echo "  TX errors:  $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/tx_errors 2>/dev/null || echo 'N/A')"
    echo "  RX dropped: $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/rx_dropped 2>/dev/null || echo 'N/A')"
    echo "  TX dropped: $(cat /sys/class/net/"$NIC_INTERFACE"/statistics/tx_dropped 2>/dev/null || echo 'N/A')"

    # Detailed ethtool stats if available
    echo ""
    echo "Ethtool Statistics (errors and drops):"
    if command -v ethtool &> /dev/null; then
        ethtool -S "$NIC_INTERFACE" 2>/dev/null | grep -iE 'error|drop|discard|miss|fail' | head -20 || echo "  No error stats available"
    fi
else
    echo "NIC statistics not available"
fi
echo ""

echo "====================================================================="
echo "Benchmark Execution Complete"
echo "====================================================================="
echo "Exit Code: $IPERF_EXIT_CODE"
echo "Completed: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

exit $IPERF_EXIT_CODE
