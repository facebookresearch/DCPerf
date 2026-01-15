# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import logging
import re
from typing import Any, Dict, List

from benchpress.lib.parser import Parser

logger: logging.Logger = logging.getLogger(__name__)


class CDNBenchParser(Parser):
    """Parser for CDN Bench and microbenchmarks"""

    def parse(
        self, stdout: List[str], stderr: List[str], returncode: int
    ) -> Dict[str, Any]:
        """
        Parse STREAM benchmark output and perf stat results from log file.
        """
        metrics: Dict[str, Any] = {"exit_code": returncode}
        if stdout[0].strip() == "MEM":
            self._parse_mem_metrics(stdout, metrics)
        elif stdout[0].strip() == "NIC":
            self._parse_nic_metrics(stdout, metrics)
        return metrics

    def _parse_mem_metrics(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """Parse MEM benchmark output.
        Args:
            stdout: stdout lines from benchmark execution
            metrics: dictionary to store metrics
        """
        patterns = {"copy": 2, "scale": 2, "add": 3, "triad": 3}
        element_size = 8
        array_size = 75000000

        # Parse MEM metrics
        for line in stdout[1:]:
            if "STREAM_ARRAY_SIZE" in line:
                metrics["stream_array_size"] = int(line.split(":")[-1].strip())
            elif "NTIMES" in line:
                metrics["stream_ntimes"] = int(line.split(":")[-1].strip())
            elif line.startswith("This system uses "):
                element_size = int(line.split()[3])
            elif line.startswith("Array size = "):
                array_size = int(line.split()[3])
            elif "Total memory required" in line:
                match = re.search(r"([\d.]+)\s+MiB", line)
                if match:
                    metrics["stream_total_memory_mib"] = float(match.group(1))

            # STREAM benchmark results
            elif any(line.startswith(pattern.title() + ":") for pattern in patterns):
                for pattern in patterns:
                    if line.startswith(pattern.title() + ":"):
                        parts = line.split()
                        if len(parts) >= 5:
                            metrics[f"{pattern}_best_MBps"] = float(parts[1])
                            num_bytes = (
                                element_size * array_size * patterns[pattern] / 1000000
                            )
                            metrics[f"{pattern}_avg_MBps"] = num_bytes / float(parts[2])
                            metrics[f"{pattern}_worst_MBps"] = num_bytes / float(
                                parts[4]
                            )
                        break

            # # Performance counters - TLB and cache(Captured in perfstat counters)
            # Timing
            elif "seconds time elapsed" in line:
                metrics["perf_time_elapsed_secs"] = float(line.split()[0])
            elif "seconds user" in line:
                metrics["perf_user_time_secs"] = float(line.split()[0])
            elif "seconds sys" in line:
                metrics["perf_sys_time_secs"] = float(line.split()[0])

            # Memory usage
            elif line.startswith("RSS Peak:"):
                match = re.search(r"VmHWM:\s+([\d]+)\s+kB", line)
                if match:
                    metrics["rss_peak_kb"] = int(match.group(1))

            # Memory Page Configuration
            elif line.startswith("Default page size:"):
                match = re.search(r"Default page size:\s+([\d]+)\s+bytes", line)
                if match:
                    metrics["default_page_size_bytes"] = int(match.group(1))
            elif re.match(r"^\s+([\d]+)kB:\s+([\d]+)\s+total,\s+([\d]+)\s+free", line):
                match = re.match(
                    r"^\s+([\d]+)kB:\s+([\d]+)\s+total,\s+([\d]+)\s+free", line
                )
                if match:
                    page_size_kb = match.group(1)
                    metrics[f"hugepages_{page_size_kb}kB_total"] = int(match.group(2))
                    metrics[f"hugepages_{page_size_kb}kB_free"] = int(match.group(3))

    def _parse_nic_metrics(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """Parse NIC benchmark output.
        Args:
            stdout: stdout lines from benchmark execution
            metrics: dictionary to store metrics
        """
        current_section = ""

        for line in stdout:
            line = line.strip()

            # Track sections
            if "NIC Information:" in line:
                current_section = "nic_info"
            elif "Network Kernel Parameters" in line:
                current_section = "network_params"
            elif "Arguments Passed" in line:
                current_section = "arguments"
            elif "iperf3 Output" in line:
                current_section = "iperf3"
            elif "Post-Benchmark NIC Statistics" in line:
                current_section = "post_stats"
            elif "iperf3 Benchmark Execution" in line:
                current_section = "execution_info"

            # Parse execution info
            if current_section == "execution_info":
                if line.startswith("Hostname:"):
                    metrics["hostname"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Kernel:"):
                    metrics["kernel_version"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Execution Date:"):
                    metrics["execution_date"] = line.split(":", 1)[-1].strip()

            # Parse NIC information
            if current_section == "nic_info":
                if line.startswith("Interface:"):
                    metrics["nic_interface"] = line.split(":", 1)[-1].strip()
                elif line.startswith("MAC Address:"):
                    metrics["nic_mac_address"] = line.split(":", 1)[-1].strip()
                elif line.startswith("MTU:"):
                    try:
                        metrics["nic_mtu"] = int(line.split(":", 1)[-1].strip())
                    except ValueError:
                        pass
                elif line.startswith("Speed:"):
                    match = re.search(r"(\d+)", line)
                    if match:
                        metrics["nic_speed_mbps"] = int(match.group(1))
                elif line.startswith("Duplex:"):
                    metrics["nic_duplex"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Operstate:"):
                    metrics["nic_operstate"] = line.split(":", 1)[-1].strip()
                elif "MSI-X IRQs:" in line:
                    match = re.search(r"MSI-X IRQs:\s*(\d+)", line)
                    if match:
                        metrics["nic_msix_irqs"] = int(match.group(1))

            # Parse network kernel parameters
            if current_section == "network_params":
                if "net.core.rmem_max:" in line:
                    match = re.search(r"net\.core\.rmem_max:\s*(\d+)", line)
                    if match:
                        metrics["net_core_rmem_max"] = int(match.group(1))
                elif "net.core.wmem_max:" in line:
                    match = re.search(r"net\.core\.wmem_max:\s*(\d+)", line)
                    if match:
                        metrics["net_core_wmem_max"] = int(match.group(1))
                elif "net.core.netdev_max_backlog:" in line:
                    match = re.search(r"net\.core\.netdev_max_backlog:\s*(\d+)", line)
                    if match:
                        metrics["net_core_netdev_max_backlog"] = int(match.group(1))
                elif "net.ipv4.tcp_congestion_control:" in line:
                    metrics["tcp_congestion_control"] = line.split(":", 1)[-1].strip()

            # Parse arguments
            if current_section == "arguments":
                if line.startswith("Mode:"):
                    metrics["mode"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Server IP:"):
                    metrics["server_ip"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Port:"):
                    metrics["port"] = line.split(":", 1)[-1].strip()
                elif line.startswith("Duration:"):
                    match = re.search(r"(\d+)", line)
                    if match:
                        metrics["duration_secs"] = int(match.group(1))
                elif line.startswith("Parallel Streams:"):
                    match = re.search(r"(\d+)", line)
                    if match:
                        metrics["parallel_streams"] = int(match.group(1))
                elif line.startswith("NUMA CPU Bind:"):
                    value = line.split(":", 1)[-1].strip()
                    if value != "none":
                        metrics["numa_cpu_bind"] = value
                elif line.startswith("NUMA Mem Bind:"):
                    value = line.split(":", 1)[-1].strip()
                    if value != "none":
                        metrics["numa_mem_bind"] = value

            # Parse iperf3 output - look for sender/receiver summary lines
            if current_section == "iperf3":
                # Match iperf3 summary lines like:
                # [SUM]   0.00-60.00  sec  52.5 GBytes  7.51 Gbits/sec  sender
                # [SUM]   0.00-60.04  sec  52.5 GBytes  7.51 Gbits/sec  receiver
                sum_match = re.search(
                    r"\[SUM\].*?([\d.]+)\s+GBytes\s+([\d.]+)\s+Gbits/sec\s+(sender|receiver)",
                    line,
                )
                if sum_match:
                    gbytes = float(sum_match.group(1))
                    gbits_sec = float(sum_match.group(2))
                    direction = sum_match.group(3)
                    metrics[f"iperf3_{direction}_gbytes"] = gbytes
                    metrics[f"iperf3_{direction}_gbits_per_sec"] = gbits_sec
                    continue

                # Also match Mbits/sec format
                sum_match_mbits = re.search(
                    r"\[SUM\].*?([\d.]+)\s+MBytes\s+([\d.]+)\s+Mbits/sec\s+(sender|receiver)",
                    line,
                )
                if sum_match_mbits:
                    mbytes = float(sum_match_mbits.group(1))
                    mbits_sec = float(sum_match_mbits.group(2))
                    direction = sum_match_mbits.group(3)
                    metrics[f"iperf3_{direction}_mbytes"] = mbytes
                    metrics[f"iperf3_{direction}_mbits_per_sec"] = mbits_sec
                    continue

                # Match retransmit count if present
                if "retrans" in line.lower() or "retr" in line.lower():
                    retr_match = re.search(r"(\d+)\s+(?:retrans|retr)", line.lower())
                    if retr_match:
                        metrics["iperf3_retransmits"] = int(retr_match.group(1))

            # Parse post-benchmark NIC statistics
            if current_section == "post_stats":
                if "RX bytes:" in line:
                    match = re.search(r"RX bytes:\s*(\d+)", line)
                    if match:
                        metrics["post_rx_bytes"] = int(match.group(1))
                elif "TX bytes:" in line:
                    match = re.search(r"TX bytes:\s*(\d+)", line)
                    if match:
                        metrics["post_tx_bytes"] = int(match.group(1))
                elif "RX packets:" in line:
                    match = re.search(r"RX packets:\s*(\d+)", line)
                    if match:
                        metrics["post_rx_packets"] = int(match.group(1))
                elif "TX packets:" in line:
                    match = re.search(r"TX packets:\s*(\d+)", line)
                    if match:
                        metrics["post_tx_packets"] = int(match.group(1))
                elif "RX errors:" in line:
                    match = re.search(r"RX errors:\s*(\d+)", line)
                    if match:
                        metrics["post_rx_errors"] = int(match.group(1))
                elif "TX errors:" in line:
                    match = re.search(r"TX errors:\s*(\d+)", line)
                    if match:
                        metrics["post_tx_errors"] = int(match.group(1))
                elif "RX dropped:" in line:
                    match = re.search(r"RX dropped:\s*(\d+)", line)
                    if match:
                        metrics["post_rx_dropped"] = int(match.group(1))
                elif "TX dropped:" in line:
                    match = re.search(r"TX dropped:\s*(\d+)", line)
                    if match:
                        metrics["post_tx_dropped"] = int(match.group(1))
