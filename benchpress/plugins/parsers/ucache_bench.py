# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import re
from typing import Any, Dict, List

from benchpress.lib.parser import Parser


def aggregate_client_results(
    results: List[Dict[str, Any]],
    include_individual: bool = True,
) -> Dict[str, Any]:
    """Aggregate results from multiple UcacheBench clients.

    This function combines metrics from multiple client instances to produce
    an overall performance summary. It handles:
    - Sum metrics: QPS, total operations, GET/SET counts, hits, misses, errors
    - Average metrics: hit ratio percentage, latency percentiles

    Args:
        results: List of parsed result dictionaries from individual clients.
                 Each dict should be the output from UcacheBenchParser.parse().
        include_individual: If True, include individual client results in output.

    Returns:
        Aggregated metrics dictionary with the following structure:
        {
            "role": "aggregated",
            "num_clients": <int>,
            "successful_clients": <int>,
            "qps": <sum of all client QPS>,
            "total_operations": <sum>,
            "get_operations": <sum>,
            "set_operations": <sum>,
            "get_hits": <sum>,
            "get_misses": <sum>,
            "get_errors": <sum>,
            "set_successes": <sum>,
            "set_errors": <sum>,
            "hit_ratio_percent": <weighted average>,
            "latency": {
                "p50": <average>,
                "p95": <average>,
                "p99": <average>,
                "p99_9": <average>,
            },
            "clients": {  # Only if include_individual=True
                "1": {...},
                "2": {...},
            }
        }

    Example:
        >>> results = [parser.parse(stdout1, stderr1, 0), parser.parse(stdout2, stderr2, 0)]
        >>> aggregated = aggregate_client_results(results)
        >>> print(f"Total QPS: {aggregated['qps']}")
    """
    if not results:
        return {
            "role": "aggregated",
            "num_clients": 0,
            "successful_clients": 0,
            "error": "No client results to aggregate",
        }

    aggregated: Dict[str, Any] = {
        "role": "aggregated",
        "num_clients": len(results),
        "successful_clients": 0,
    }

    # Metrics that should be summed across clients
    sum_metrics = [
        "qps",
        "total_operations",
        "get_operations",
        "set_operations",
        "get_hits",
        "get_misses",
        "get_errors",
        "set_successes",
        "set_errors",
    ]

    # Initialize sum metrics to 0
    for metric in sum_metrics:
        aggregated[metric] = 0

    # Metrics that should be averaged (weighted by operations if possible)
    # Track values for averaging
    hit_ratio_values: List[float] = []
    hit_ratio_weights: List[int] = []  # Weight by get_operations for accurate average

    latency_values: Dict[str, List[float]] = {
        "p50": [],
        "p95": [],
        "p99": [],
        "p99_9": [],
    }

    # Track duration for weighted averaging
    duration_values: List[float] = []

    # Process each client result
    for result in results:
        # Check if this is a valid result (has QPS or operations data)
        is_valid = result.get("qps", 0) > 0 or result.get("total_operations", 0) > 0

        if is_valid:
            aggregated["successful_clients"] += 1

        # Sum metrics
        for metric in sum_metrics:
            if metric in result:
                aggregated[metric] += result[metric]

        # Collect hit ratio for weighted averaging
        if "hit_ratio_percent" in result:
            hit_ratio_values.append(result["hit_ratio_percent"])
            # Weight by get_operations if available, otherwise equal weight
            weight = result.get("get_operations", 1)
            hit_ratio_weights.append(weight if weight > 0 else 1)

        # Collect latency percentiles for averaging
        if "latency" in result:
            latency = result["latency"]
            for key in latency_values:
                if key in latency:
                    latency_values[key].append(latency[key])

        # Collect duration
        if "duration_seconds" in result:
            duration_values.append(result["duration_seconds"])

    # Calculate weighted average hit ratio
    if hit_ratio_values and hit_ratio_weights:
        total_weight = sum(hit_ratio_weights)
        if total_weight > 0:
            weighted_sum = sum(
                v * w for v, w in zip(hit_ratio_values, hit_ratio_weights)
            )
            aggregated["hit_ratio_percent"] = round(weighted_sum / total_weight, 2)
        else:
            aggregated["hit_ratio_percent"] = round(
                sum(hit_ratio_values) / len(hit_ratio_values), 2
            )

    # Calculate average latencies
    latencies: Dict[str, float] = {}
    for key, values in latency_values.items():
        if values:
            latencies[key] = round(sum(values) / len(values), 3)
    if latencies:
        aggregated["latency"] = latencies

    # Calculate average duration
    if duration_values:
        aggregated["duration_seconds"] = round(
            sum(duration_values) / len(duration_values), 2
        )

    # Include individual client results if requested
    if include_individual:
        clients: Dict[str, Dict[str, Any]] = {}
        for idx, result in enumerate(results):
            client_key = str(idx + 1)  # 1-indexed for readability
            clients[client_key] = result
        aggregated["clients"] = clients

    # Add summary statistics
    if aggregated["successful_clients"] > 0:
        aggregated["avg_qps_per_client"] = round(
            aggregated["qps"] / aggregated["successful_clients"], 2
        )

    return aggregated


def aggregate_client_results_from_json(
    json_results: List[str],
    include_individual: bool = True,
) -> Dict[str, Any]:
    """Aggregate client results from JSON strings.

    This is a convenience function for aggregating results that have been
    serialized to JSON (e.g., collected from remote clients).

    Args:
        json_results: List of JSON strings, each representing a client result.
        include_individual: If True, include individual client results in output.

    Returns:
        Aggregated metrics dictionary (same format as aggregate_client_results).

    Raises:
        json.JSONDecodeError: If any JSON string is invalid.
    """
    results = [json.loads(r) for r in json_results]
    return aggregate_client_results(results, include_individual)


class UcacheBenchParser(Parser):
    """Parser for UcacheBench output.

    Example output:
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
    """

    def parse(
        self, stdout: List[str], stderr: List[str], returncode: int
    ) -> Dict[str, Any]:
        """Parse UcacheBench output and extract performance metrics."""
        # Join all output lines for easier parsing
        output = "\n".join(stdout + stderr)

        metrics: Dict[str, Any] = {}

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
