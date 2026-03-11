# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import re
from typing import Any, Dict, List

from benchpress.lib.parser import Parser


class UcacheBenchParser(Parser):
    """Parser for UcacheBench output.

    Parses the text output from either:
    1. The ucachebench_client binary (single-client mode)
    2. The ucachebench_server binary (multi-client aggregated results)

    Example client output:
    WARMUP PHASE:
      Status: ✓ SUCCESS
      Duration: 10.00 seconds
      Operations: 50000 (5000.0 QPS)
      SET Successes: 50000
      SET Errors: 0
      Success Rate: 100.0%

    BENCHMARK PHASE:
      Duration: 60.00 seconds
      Total Operations: 600000
      QPS: 10000.0

    GET Operations: 540000
      Hits: 432000
      Misses: 108000
      Errors: 0
      Hit Ratio: 80.00%

    SET Operations: 60000
      Successes: 60000
      Errors: 0

    Latency Percentiles (ms):
      P50: 0.05
      P95: 0.12
      P99: 0.25
      P99.9: 0.50

    Example server output (multi-client aggregated):
    ========================================
         UCACHEBENCH SERVER RESULTS
    ========================================
    Benchmark Duration: 60.000 seconds

    Operations:
      GET requests:    540000
      GET hits:        432000
      GET misses:      108000
      SET requests:    60000
      DELETE requests: 0
      Total ops:       600000

    Performance:
      QPS:        10000.0
      Hit Ratio:  80.00%
    ========================================
    """

    def parse(
        self, stdout: List[str], stderr: List[str], returncode: int
    ) -> Dict[str, Any]:
        """Parse UcacheBench output and extract performance metrics."""
        # Join all output lines for easier parsing
        output = "\n".join(stdout + stderr)

        metrics: Dict[str, Any] = {}

        # Detect if this is server-side aggregated output
        is_server_output = "UCACHEBENCH SERVER RESULTS" in output

        if is_server_output:
            # Parse server-side aggregated results
            server_metrics = self._parse_server_results(output)
            if server_metrics:
                metrics.update(server_metrics)
        else:
            # Parse client-side output (original format)
            # Parse warmup phase metrics
            warmup_metrics = self._parse_warmup_phase(output)
            if warmup_metrics:
                metrics["warmup"] = warmup_metrics

            # Parse benchmark phase metrics
            benchmark_metrics = self._parse_benchmark_phase(output)
            if benchmark_metrics:
                metrics.update(benchmark_metrics)

        # Add exit code
        metrics["exit_code"] = returncode

        return metrics

    def _parse_server_results(self, output: str) -> Dict[str, Any]:
        """Parse server-side aggregated results (multi-client mode)."""
        metrics: Dict[str, Any] = {}

        # Parse benchmark duration
        duration_match = re.search(r"Benchmark Duration: ([\d.]+) seconds", output)
        if duration_match:
            metrics["duration_seconds"] = float(duration_match.group(1))

        # Parse GET operations
        get_reqs_match = re.search(r"GET requests:\s+(\d+)", output)
        if get_reqs_match:
            metrics["get_operations"] = int(get_reqs_match.group(1))

        get_hits_match = re.search(r"GET hits:\s+(\d+)", output)
        if get_hits_match:
            metrics["get_hits"] = int(get_hits_match.group(1))

        get_misses_match = re.search(r"GET misses:\s+(\d+)", output)
        if get_misses_match:
            metrics["get_misses"] = int(get_misses_match.group(1))

        # Parse SET operations
        set_reqs_match = re.search(r"SET requests:\s+(\d+)", output)
        if set_reqs_match:
            metrics["set_operations"] = int(set_reqs_match.group(1))

        # Parse DELETE operations
        delete_reqs_match = re.search(r"DELETE requests:\s+(\d+)", output)
        if delete_reqs_match:
            metrics["delete_operations"] = int(delete_reqs_match.group(1))

        # Parse total operations
        total_ops_match = re.search(r"Total ops:\s+(\d+)", output)
        if total_ops_match:
            metrics["total_operations"] = int(total_ops_match.group(1))

        # Parse QPS from the "Performance:" section of the final results block.
        # Must be anchored to "Performance:" to avoid matching periodic stats
        # lines like "[Server BENCHMARK] ... | QPS: ... |" which appear earlier
        # in the output and can contain transient spike values.
        qps_match = re.search(r"Performance:\s*\n\s*QPS:\s+([\d.]+)", output, re.DOTALL)
        if qps_match:
            metrics["qps"] = float(qps_match.group(1))

        # Parse hit ratio from the "Performance:" section for the same reason
        hit_ratio_match = re.search(
            r"Performance:\s*\n\s*QPS:\s+[\d.]+\s*\n\s*Hit Ratio:\s+([\d.]+)%",
            output,
            re.DOTALL,
        )
        if hit_ratio_match:
            metrics["hit_ratio_percent"] = float(hit_ratio_match.group(1))

        # Mark this as server-aggregated results
        metrics["source"] = "server_aggregated"

        return metrics

    def _parse_warmup_phase(self, output: str) -> Dict[str, Any]:
        """Parse warmup phase metrics."""
        warmup_metrics: Dict[str, Any] = {}

        # Check if warmup was successful
        if "Status: ✓ SUCCESS" in output:
            warmup_metrics["success"] = True
        elif "Status: ✗ FAILED" in output:
            warmup_metrics["success"] = False
        elif "Status: Disabled" in output:
            warmup_metrics["success"] = None  # Disabled

        # Parse warmup duration
        duration_match = re.search(
            r"WARMUP PHASE:.*?Duration: ([\d.]+) seconds", output, re.DOTALL
        )
        if duration_match:
            warmup_metrics["duration_seconds"] = float(duration_match.group(1))

        # Parse warmup operations and QPS
        ops_match = re.search(
            r"WARMUP PHASE:.*?Operations: (\d+) \(([\d.]+) QPS\)", output, re.DOTALL
        )
        if ops_match:
            warmup_metrics["total_operations"] = int(ops_match.group(1))
            warmup_metrics["qps"] = float(ops_match.group(2))

        # Parse SET operations in warmup
        set_success_match = re.search(
            r"WARMUP PHASE:.*?SET Successes: (\d+)", output, re.DOTALL
        )
        if set_success_match:
            warmup_metrics["set_successes"] = int(set_success_match.group(1))

        set_errors_match = re.search(
            r"WARMUP PHASE:.*?SET Errors: (\d+)", output, re.DOTALL
        )
        if set_errors_match:
            warmup_metrics["set_errors"] = int(set_errors_match.group(1))

        # Parse success rate
        success_rate_match = re.search(
            r"WARMUP PHASE:.*?Success Rate: ([\d.]+)%", output, re.DOTALL
        )
        if success_rate_match:
            warmup_metrics["success_rate_percent"] = float(success_rate_match.group(1))

        return warmup_metrics

    def _parse_benchmark_phase(self, output: str) -> Dict[str, Any]:
        """Parse benchmark phase metrics."""
        metrics: Dict[str, Any] = {}

        # Parse benchmark duration and overall QPS
        duration_match = re.search(
            r"BENCHMARK PHASE:.*?Duration: ([\d.]+) seconds", output, re.DOTALL
        )
        if duration_match:
            metrics["duration_seconds"] = float(duration_match.group(1))

        total_ops_match = re.search(
            r"BENCHMARK PHASE:.*?Total Operations: (\d+)", output, re.DOTALL
        )
        if total_ops_match:
            metrics["total_operations"] = int(total_ops_match.group(1))

        qps_match = re.search(r"BENCHMARK PHASE:.*?QPS: ([\d.]+)", output, re.DOTALL)
        if qps_match:
            metrics["qps"] = float(qps_match.group(1))

        # Parse GET operations
        get_ops_match = re.search(r"GET Operations: (\d+)", output)
        if get_ops_match:
            metrics["get_operations"] = int(get_ops_match.group(1))

        get_hits_match = re.search(r"GET Operations:.*?Hits: (\d+)", output, re.DOTALL)
        if get_hits_match:
            metrics["get_hits"] = int(get_hits_match.group(1))

        get_misses_match = re.search(
            r"GET Operations:.*?Misses: (\d+)", output, re.DOTALL
        )
        if get_misses_match:
            metrics["get_misses"] = int(get_misses_match.group(1))

        get_errors_match = re.search(
            r"GET Operations:.*?Errors: (\d+)", output, re.DOTALL
        )
        if get_errors_match:
            metrics["get_errors"] = int(get_errors_match.group(1))

        hit_ratio_match = re.search(r"Hit Ratio: ([\d.]+)%", output)
        if hit_ratio_match:
            metrics["hit_ratio_percent"] = float(hit_ratio_match.group(1))

        # Parse SET operations
        set_ops_match = re.search(r"SET Operations: (\d+)", output)
        if set_ops_match:
            metrics["set_operations"] = int(set_ops_match.group(1))

        set_successes_match = re.search(
            r"SET Operations:.*?Successes: (\d+)", output, re.DOTALL
        )
        if set_successes_match:
            metrics["set_successes"] = int(set_successes_match.group(1))

        set_errors_match = re.search(
            r"SET Operations:.*?Errors: (\d+)", output, re.DOTALL
        )
        if set_errors_match:
            metrics["set_errors"] = int(set_errors_match.group(1))

        # Parse latency percentiles
        latencies = self._parse_latency_percentiles(output)
        if latencies:
            metrics["latency"] = latencies

        return metrics

    def _parse_latency_percentiles(self, output: str) -> Dict[str, float]:
        """Parse latency percentile metrics."""
        latencies: Dict[str, float] = {}

        # Parse P50, P95, P99, P99.9 latencies
        percentiles = ["P50", "P95", "P99", "P99.9"]

        for percentile in percentiles:
            pattern = rf"{percentile}: ([\d.]+)"
            match = re.search(pattern, output)
            if match:
                # Convert percentile name to safe key (P99.9 -> p99_9)
                key = percentile.lower().replace(".", "_")
                latencies[key] = float(match.group(1))

        return latencies
