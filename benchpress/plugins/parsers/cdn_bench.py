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
            return metrics  # Implement NIC parsing
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

            # Cache hierarchy(Captured in system spec output file)

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

            # CPU utilization (Captured in mpstat.log)
