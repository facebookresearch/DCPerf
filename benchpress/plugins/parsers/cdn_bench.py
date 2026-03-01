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
        elif stdout[0].strip() == "FLASH":
            self._parse_flash_metrics(stdout, metrics)
        elif stdout[0].strip() == "CPU":
            self._parse_cpu_metrics(stdout, metrics)
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

    def _parse_flash_metrics(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """
        Parse Flash/FIO benchmark output including JSON results.
        Args:
            stdout: stdout lines from fio benchmark execution
            metrics: dictionary to store metrics
        """
        import json

        # Join all lines to find JSON block
        full_output = "\n".join(stdout)

        # Parse configuration from log output
        for line in stdout:
            line_stripped = line.strip()

            # Parse execution info
            if line_stripped.startswith("Execution Date:"):
                metrics["execution_date"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("FIO Version:"):
                metrics["fio_version"] = line_stripped.split(":", 1)[-1].strip()

            # Parse configuration
            elif line_stripped.startswith("- Device:"):
                metrics["target_device"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("- Directory:"):
                metrics["target_directory"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("- Access Pattern:"):
                metrics["access_pattern"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("- Block Size:"):
                metrics["block_size"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("- Number of Jobs:"):
                try:
                    metrics["num_jobs"] = int(line_stripped.split(":", 1)[-1].strip())
                except ValueError:
                    pass
            elif line_stripped.startswith("- I/O Depth:"):
                try:
                    metrics["io_depth"] = int(line_stripped.split(":", 1)[-1].strip())
                except ValueError:
                    pass
            elif line_stripped.startswith("- I/O Engine:"):
                metrics["io_engine"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("- Runtime:"):
                match = re.search(r"(\d+)", line_stripped)
                if match:
                    metrics["runtime_secs"] = int(match.group(1))
            elif line_stripped.startswith("- Ramp Time:"):
                match = re.search(r"(\d+)", line_stripped)
                if match:
                    metrics["ramp_time_secs"] = int(match.group(1))

            # Parse NVMe device info
            elif "SAMSUNG" in line_stripped or "INTEL" in line_stripped:
                parts = line_stripped.split()
                if len(parts) >= 4:
                    metrics["nvme_model"] = (
                        " ".join(parts[3:5]) if len(parts) > 4 else parts[3]
                    )

            # Parse NVMe SMART data
            elif line_stripped.startswith("temperature"):
                match = re.search(r"(\d+)\s*°F", line_stripped)
                if match:
                    metrics["nvme_temp_fahrenheit"] = int(match.group(1))
            elif line_stripped.startswith("available_spare"):
                match = re.search(r"(\d+)%", line_stripped)
                if match:
                    metrics["nvme_available_spare_pct"] = int(match.group(1))
            elif line_stripped.startswith("percentage_used"):
                match = re.search(r"(\d+)%", line_stripped)
                if match:
                    metrics["nvme_percentage_used"] = int(match.group(1))
            elif line_stripped.startswith("power_on_hours"):
                match = re.search(r":\s*(\d+)", line_stripped)
                if match:
                    metrics["nvme_power_on_hours"] = int(match.group(1))
            elif line_stripped.startswith("media_errors"):
                match = re.search(r":\s*(\d+)", line_stripped)
                if match:
                    metrics["nvme_media_errors"] = int(match.group(1))

            # Parse I/O scheduler
            elif line_stripped.startswith("[none]") or line_stripped.startswith(
                "[mq-deadline]"
            ):
                metrics["io_scheduler"] = line_stripped

        # Extract and parse FIO JSON output
        try:
            json_start = full_output.find('{\n  "fio version"')
            if json_start == -1:
                json_start = full_output.find('{"fio version"')
            if json_start != -1:
                # Find the end of JSON by counting braces
                brace_count = 0
                json_end = json_start
                for i, char in enumerate(full_output[json_start:]):
                    if char == "{":
                        brace_count += 1
                    elif char == "}":
                        brace_count -= 1
                        if brace_count == 0:
                            json_end = json_start + i + 1
                            break

                json_str = full_output[json_start:json_end]
                fio_data = json.loads(json_str)

                # Parse FIO version and timestamp
                if "fio version" in fio_data:
                    metrics["fio_version"] = fio_data["fio version"]
                if "timestamp" in fio_data:
                    metrics["fio_timestamp"] = fio_data["timestamp"]

                # Parse job results
                if "jobs" in fio_data and len(fio_data["jobs"]) > 0:
                    job = fio_data["jobs"][0]

                    # Job runtime and CPU
                    if "job_runtime" in job:
                        metrics["job_runtime_ms"] = job["job_runtime"]
                    if "usr_cpu" in job:
                        metrics["usr_cpu_pct"] = job["usr_cpu"]
                    if "sys_cpu" in job:
                        metrics["sys_cpu_pct"] = job["sys_cpu"]
                    if "ctx" in job:
                        metrics["context_switches"] = job["ctx"]

                    # Parse READ metrics
                    if "read" in job:
                        read_data = job["read"]
                        if read_data.get("io_bytes", 0) > 0:
                            metrics["read_io_bytes"] = read_data["io_bytes"]
                            metrics["read_bw_bytes_sec"] = read_data.get("bw_bytes", 0)
                            metrics["read_bw_kbps"] = read_data.get("bw", 0)
                            metrics["read_iops"] = read_data.get("iops", 0)
                            metrics["read_runtime_ms"] = read_data.get("runtime", 0)
                            metrics["read_total_ios"] = read_data.get("total_ios", 0)

                            # Latency stats
                            if "clat_ns" in read_data:
                                clat = read_data["clat_ns"]
                                metrics["read_clat_min_ns"] = clat.get("min", 0)
                                metrics["read_clat_max_ns"] = clat.get("max", 0)
                                metrics["read_clat_mean_ns"] = clat.get("mean", 0)
                                metrics["read_clat_stddev_ns"] = clat.get("stddev", 0)

                                # Percentiles
                                if "percentile" in clat:
                                    pct = clat["percentile"]
                                    metrics["read_clat_p50_ns"] = pct.get(
                                        "50.000000", 0
                                    )
                                    metrics["read_clat_p90_ns"] = pct.get(
                                        "90.000000", 0
                                    )
                                    metrics["read_clat_p95_ns"] = pct.get(
                                        "95.000000", 0
                                    )
                                    metrics["read_clat_p99_ns"] = pct.get(
                                        "99.000000", 0
                                    )
                                    metrics["read_clat_p999_ns"] = pct.get(
                                        "99.900000", 0
                                    )
                                    metrics["read_clat_p9999_ns"] = pct.get(
                                        "99.990000", 0
                                    )

                            # IOPS stats
                            metrics["read_iops_min"] = read_data.get("iops_min", 0)
                            metrics["read_iops_max"] = read_data.get("iops_max", 0)
                            metrics["read_iops_mean"] = read_data.get("iops_mean", 0)
                            metrics["read_iops_stddev"] = read_data.get(
                                "iops_stddev", 0
                            )

                            # BW stats
                            metrics["read_bw_min_kbps"] = read_data.get("bw_min", 0)
                            metrics["read_bw_max_kbps"] = read_data.get("bw_max", 0)
                            metrics["read_bw_mean_kbps"] = read_data.get("bw_mean", 0)

                    # Parse WRITE metrics
                    if "write" in job:
                        write_data = job["write"]
                        if write_data.get("io_bytes", 0) > 0:
                            metrics["write_io_bytes"] = write_data["io_bytes"]
                            metrics["write_bw_bytes_sec"] = write_data.get(
                                "bw_bytes", 0
                            )
                            metrics["write_bw_kbps"] = write_data.get("bw", 0)
                            metrics["write_iops"] = write_data.get("iops", 0)
                            metrics["write_runtime_ms"] = write_data.get("runtime", 0)
                            metrics["write_total_ios"] = write_data.get("total_ios", 0)

                            # Latency stats
                            if "clat_ns" in write_data:
                                clat = write_data["clat_ns"]
                                metrics["write_clat_min_ns"] = clat.get("min", 0)
                                metrics["write_clat_max_ns"] = clat.get("max", 0)
                                metrics["write_clat_mean_ns"] = clat.get("mean", 0)
                                metrics["write_clat_stddev_ns"] = clat.get("stddev", 0)

                                # Percentiles
                                if "percentile" in clat:
                                    pct = clat["percentile"]
                                    metrics["write_clat_p50_ns"] = pct.get(
                                        "50.000000", 0
                                    )
                                    metrics["write_clat_p90_ns"] = pct.get(
                                        "90.000000", 0
                                    )
                                    metrics["write_clat_p95_ns"] = pct.get(
                                        "95.000000", 0
                                    )
                                    metrics["write_clat_p99_ns"] = pct.get(
                                        "99.000000", 0
                                    )
                                    metrics["write_clat_p999_ns"] = pct.get(
                                        "99.900000", 0
                                    )
                                    metrics["write_clat_p9999_ns"] = pct.get(
                                        "99.990000", 0
                                    )

                            # IOPS stats
                            metrics["write_iops_min"] = write_data.get("iops_min", 0)
                            metrics["write_iops_max"] = write_data.get("iops_max", 0)
                            metrics["write_iops_mean"] = write_data.get("iops_mean", 0)
                            metrics["write_iops_stddev"] = write_data.get(
                                "iops_stddev", 0
                            )

                            # BW stats
                            metrics["write_bw_min_kbps"] = write_data.get("bw_min", 0)
                            metrics["write_bw_max_kbps"] = write_data.get("bw_max", 0)
                            metrics["write_bw_mean_kbps"] = write_data.get("bw_mean", 0)

                    # Latency distribution buckets
                    if "latency_ms" in job:
                        lat_ms = job["latency_ms"]
                        metrics["lat_pct_under_2ms"] = lat_ms.get("2", 0)
                        metrics["lat_pct_under_4ms"] = lat_ms.get("4", 0)
                        metrics["lat_pct_under_10ms"] = lat_ms.get("10", 0)
                        metrics["lat_pct_under_20ms"] = lat_ms.get("20", 0)
                        metrics["lat_pct_under_50ms"] = lat_ms.get("50", 0)

                # Parse disk utilization
                if "disk_util" in fio_data and len(fio_data["disk_util"]) > 0:
                    disk = fio_data["disk_util"][0]
                    metrics["disk_name"] = disk.get("name", "")
                    metrics["disk_read_ios"] = disk.get("read_ios", 0)
                    metrics["disk_write_ios"] = disk.get("write_ios", 0)
                    metrics["disk_read_sectors"] = disk.get("read_sectors", 0)
                    metrics["disk_write_sectors"] = disk.get("write_sectors", 0)
                    metrics["disk_util_pct"] = disk.get("util", 0)
                    metrics["disk_in_queue_ms"] = disk.get("in_queue", 0)

        except json.JSONDecodeError as e:
            logger.warning(f"Failed to parse FIO JSON output: {e}")
        except Exception as e:
            logger.warning(f"Error parsing flash metrics: {e}")

    def _parse_cpu_metrics(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """
        Parse CPU benchmark output from stress-ng.
        Extracts metrics from inline YAML output and metrics-brief table.
        Args:
            stdout: stdout lines from stress-ng benchmark execution
            metrics: dictionary to store metrics
        """
        self._parse_cpu_config(stdout, metrics)
        self._parse_cpu_metrics_brief(stdout, metrics)
        self._parse_cpu_yaml(stdout, metrics)
        self._parse_cpu_times(stdout, metrics)

    def _parse_cpu_config(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """Parse configuration and system info from stress-ng run output."""
        current_section = ""

        for line in stdout:
            line_stripped = line.strip()

            # Track sections
            if "CPU Governor Configuration" in line_stripped:
                current_section = "governor"
            elif "System Information" in line_stripped:
                current_section = "sysinfo"
            elif "Configuration" in line_stripped and "===" in line:
                current_section = "config"
            elif "Running stress-ng" in line_stripped:
                current_section = "running"
            elif "Post-Benchmark" in line_stripped:
                current_section = "post"

            # Parse governor info
            if current_section == "governor":
                if "Setting CPU governor to:" in line_stripped:
                    metrics["cpu_governor"] = line_stripped.split(":")[-1].strip()

            # Parse system info
            if current_section == "sysinfo":
                if line_stripped.startswith("Hostname:"):
                    metrics["hostname"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("Kernel:"):
                    metrics["kernel_version"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("Architecture:"):
                    metrics["architecture"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("CPU Model:"):
                    metrics["cpu_model"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("Physical Cores:"):
                    try:
                        metrics["physical_cores"] = int(
                            line_stripped.split(":", 1)[-1].strip()
                        )
                    except ValueError:
                        pass
                elif line_stripped.startswith("NUMA Nodes:"):
                    try:
                        metrics["numa_nodes"] = int(
                            line_stripped.split(":", 1)[-1].strip()
                        )
                    except ValueError:
                        pass

            # Parse configuration
            if current_section == "config":
                if line_stripped.startswith("Stressor:"):
                    metrics["stressor"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("Workers:"):
                    try:
                        metrics["workers"] = int(
                            line_stripped.split(":", 1)[-1].strip()
                        )
                    except ValueError:
                        pass
                elif line_stripped.startswith("Timeout:"):
                    match = re.search(r"(\d+)", line_stripped)
                    if match:
                        metrics["timeout_secs"] = int(match.group(1))
                elif line_stripped.startswith("CPU Method:"):
                    metrics["cpu_method"] = line_stripped.split(":", 1)[-1].strip()
                elif line_stripped.startswith("Matrix Size:"):
                    try:
                        metrics["matrix_size"] = int(
                            line_stripped.split(":", 1)[-1].strip()
                        )
                    except ValueError:
                        pass

            # Parse stress-ng version
            if line_stripped.startswith("stress-ng Version:"):
                metrics["stress_ng_version"] = line_stripped.split(":", 1)[-1].strip()
            elif line_stripped.startswith("Execution Date:"):
                metrics["execution_date"] = line_stripped.split(":", 1)[-1].strip()

    def _parse_cpu_metrics_brief(
        self, stdout: List[str], metrics: Dict[str, Any]
    ) -> None:
        """Parse stress-ng --metrics-brief table output.

        Expected format (from stderr, captured via 2>&1):
        stress-ng: info: [...] stressor  bogo ops real time ...
        stress-ng: info: [...] cpu       8640000    60.00 ...
        """
        for line in stdout:
            line_stripped = line.strip()

            # Match metrics-brief data lines
            # Format: stress-ng: info:  [PID] <stressor> <bogo_ops> <real_time>
            #         <usr_time> <sys_time> <bogo_ops_rt> <bogo_ops_ust> <cpu_pct>
            brief_match = re.search(
                r"stress-ng:\s+info:\s+\[\d+\]\s+"
                r"(\w+)\s+"
                r"(\d+)\s+"
                r"([\d.]+)\s+"
                r"([\d.]+)\s+"
                r"([\d.]+)\s+"
                r"([\d.]+)\s+"
                r"([\d.]+)\s+"
                r"([\d.]+)",
                line_stripped,
            )
            if brief_match:
                stressor_name = brief_match.group(1)
                # Skip header lines
                if stressor_name in ("stressor", "info"):
                    continue
                metrics["brief_stressor"] = stressor_name
                metrics["brief_bogo_ops"] = int(brief_match.group(2))
                metrics["brief_real_time_secs"] = float(brief_match.group(3))
                metrics["brief_usr_time_secs"] = float(brief_match.group(4))
                metrics["brief_sys_time_secs"] = float(brief_match.group(5))
                metrics["brief_bogo_ops_per_sec_real"] = float(brief_match.group(6))
                metrics["brief_bogo_ops_per_sec_usr_sys"] = float(brief_match.group(7))
                metrics["brief_cpu_used_per_instance_pct"] = float(brief_match.group(8))

            # Parse successful run completion
            if "successful run completed in" in line_stripped:
                match = re.search(r"completed in ([\d.]+)s", line_stripped)
                if match:
                    metrics["total_run_time_secs"] = float(match.group(1))

    def _parse_cpu_yaml(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """Parse stress-ng YAML output block delimited by markers."""
        import yaml

        full_output = "\n".join(stdout)

        # Extract YAML block between markers
        yaml_start = full_output.find("BEGIN_STRESS_NG_YAML")
        yaml_end = full_output.find("END_STRESS_NG_YAML")

        if yaml_start == -1 or yaml_end == -1:
            return

        # Skip the marker line itself
        yaml_start = full_output.index("\n", yaml_start) + 1
        yaml_str = full_output[yaml_start:yaml_end].strip()

        if not yaml_str:
            return

        try:
            data = yaml.safe_load(yaml_str)
            if not isinstance(data, dict) or "stress-ng" not in data:
                return

            stress_data = data["stress-ng"]

            if "system-info" in stress_data:
                self._extract_yaml_sysinfo(stress_data["system-info"], metrics)
            if "metrics" in stress_data:
                self._extract_yaml_stressor_metrics(stress_data["metrics"], metrics)
            if "times" in stress_data and isinstance(stress_data["times"], dict):
                if "run-time" in stress_data["times"]:
                    metrics["yaml_run_time_secs"] = float(
                        stress_data["times"]["run-time"]
                    )

        except yaml.YAMLError as e:
            logger.warning(f"Failed to parse stress-ng YAML output: {e}")
        except Exception as e:
            logger.warning(f"Error parsing stress-ng YAML metrics: {e}")

    def _extract_yaml_sysinfo(
        self, sysinfo: Dict[str, Any], metrics: Dict[str, Any]
    ) -> None:
        """Extract system-info fields from stress-ng YAML."""
        if "stress-ng-version" in sysinfo:
            metrics["stress_ng_version"] = str(sysinfo["stress-ng-version"])
        if "num-cpus-online" in sysinfo:
            metrics["yaml_cpus_online"] = int(sysinfo["num-cpus-online"])
        if "compiler" in sysinfo:
            metrics["compiler"] = str(sysinfo["compiler"])

    def _extract_yaml_stressor_metrics(
        self, stressor_metrics: List[Dict[str, Any]], metrics: Dict[str, Any]
    ) -> None:
        """Extract per-stressor metrics from stress-ng YAML."""
        yaml_metric_map = {
            "bogo-ops": ("bogo_ops", int),
            "bogo-ops-per-second-usr-sys-time": ("bogo_ops_per_sec_usr_sys", float),
            "bogo-ops-per-second-real-time": ("bogo_ops_per_sec_real", float),
            "wall-clock-time": ("wall_clock_time_secs", float),
            "user-time": ("user_time_secs", float),
            "system-time": ("system_time_secs", float),
            "cpu-usage-per-instance": ("cpu_usage_per_instance_pct", float),
        }
        for entry in stressor_metrics:
            if not isinstance(entry, dict):
                continue
            metrics["yaml_stressor"] = entry.get("stressor", "unknown")
            for yaml_key, (metric_name, converter) in yaml_metric_map.items():
                if yaml_key in entry:
                    metrics[metric_name] = converter(entry[yaml_key])

    def _parse_cpu_times(self, stdout: List[str], metrics: Dict[str, Any]) -> None:
        """Parse stress-ng --times output for time breakdown."""
        for line in stdout:
            line_stripped = line.strip()

            # Parse times output lines like:
            # stress-ng: info: [PID]   4320.00s available CPU time
            # stress-ng: info: [PID]   4319.78s user time   ( 99.99%)
            # stress-ng: info: [PID]      0.22s system time (  0.01%)

            if "available CPU time" in line_stripped:
                match = re.search(r"([\d.]+)s\s+available CPU time", line_stripped)
                if match:
                    metrics["available_cpu_time_secs"] = float(match.group(1))

            elif "user time" in line_stripped and "stress-ng" in line_stripped:
                match = re.search(
                    r"([\d.]+)s\s+user time\s+\(\s*([\d.]+)%\)", line_stripped
                )
                if match:
                    metrics["times_user_secs"] = float(match.group(1))
                    metrics["times_user_pct"] = float(match.group(2))

            elif "system time" in line_stripped and "stress-ng" in line_stripped:
                match = re.search(
                    r"([\d.]+)s\s+system time\s+\(\s*([\d.]+)%\)", line_stripped
                )
                if match:
                    metrics["times_system_secs"] = float(match.group(1))
                    metrics["times_system_pct"] = float(match.group(2))

            elif "total time" in line_stripped and "stress-ng" in line_stripped:
                match = re.search(
                    r"([\d.]+)s\s+total time\s+\(\s*([\d.]+)%\)", line_stripped
                )
                if match:
                    metrics["times_total_secs"] = float(match.group(1))
                    metrics["times_total_pct"] = float(match.group(2))

            elif "stressors started" in line_stripped:
                match = re.search(r"(\d+)\s+stressors started", line_stripped)
                if match:
                    metrics["stressors_started"] = int(match.group(1))
