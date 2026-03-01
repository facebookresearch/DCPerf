#!/usr/bin/env python3
# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Parse production ucache traffic distribution CSV and generate configuration
for ucache_bench client to simulate production-like traffic patterns.
"""

import argparse
import csv
import json
from dataclasses import dataclass
from typing import Dict, List


@dataclass
class OperationStats:
    """Statistics for a single operation type"""

    operation: str
    hits: int
    samples: int
    request_wire_bytes_avg: float
    request_wire_value_bytes_avg: float
    request_wire_value_bytes_p50: float
    request_wire_value_bytes_p75: float
    request_wire_value_bytes_p95: float
    request_wire_value_bytes_p99: float
    reply_size_before_compression_avg: float
    reply_wire_value_bytes_avg: float
    reply_wire_value_bytes_p50: float
    reply_wire_value_bytes_p75: float
    reply_wire_value_bytes_p95: float
    reply_wire_value_bytes_p99: float
    key_size_avg: float


@dataclass
class DistributionConfig:
    """Configuration for traffic distribution"""

    get_ratio: float
    get_key_size_avg: float
    get_response_size_avg: float
    get_response_size_p50: float
    get_response_size_p75: float
    get_response_size_p95: float
    get_response_size_p99: float
    set_key_size_avg: float
    set_value_size_avg: float
    set_value_size_p50: float
    set_value_size_p75: float
    set_value_size_p95: float
    set_value_size_p99: float


def parse_csv(csv_file: str) -> List[OperationStats]:
    """Parse traffic distribution CSV file"""
    operations: List[OperationStats] = []

    with open(csv_file, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            op = OperationStats(
                operation=row["operation"],
                hits=int(float(row["Hits"])),
                samples=int(float(row["Samples"])),
                request_wire_bytes_avg=float(row["request_wire_bytes (avg)"]),
                request_wire_value_bytes_avg=float(
                    row["request_wire_value_bytes (avg)"]
                ),
                request_wire_value_bytes_p50=float(
                    row["request_wire_value_bytes (p50)"]
                ),
                request_wire_value_bytes_p75=float(
                    row["request_wire_value_bytes (p75)"]
                ),
                request_wire_value_bytes_p95=float(
                    row["request_wire_value_bytes (p95)"]
                ),
                request_wire_value_bytes_p99=float(
                    row["request_wire_value_bytes (p99)"]
                ),
                reply_size_before_compression_avg=float(
                    row["reply_size_before_compression (avg)"]
                ),
                reply_wire_value_bytes_avg=float(row["reply_wire_value_bytes (avg)"]),
                reply_wire_value_bytes_p50=float(row["reply_wire_value_bytes (p50)"]),
                reply_wire_value_bytes_p75=float(row["reply_wire_value_bytes (p75)"]),
                reply_wire_value_bytes_p95=float(row["reply_wire_value_bytes (p95)"]),
                reply_wire_value_bytes_p99=float(row["reply_wire_value_bytes (p99)"]),
                key_size_avg=float(row["key_size (avg)"]),
            )
            operations.append(op)

    return operations


def categorize_operations(
    operations: List[OperationStats],
) -> Dict[str, List[OperationStats]]:
    """Categorize operations into GET, SET, and OTHER"""
    get_ops = []
    set_ops = []
    other_ops = []

    for op in operations:
        op_name_lower = op.operation.lower()

        # GET-related operations
        if any(
            keyword in op_name_lower
            for keyword in ["get", "gets", "lease-get", "multifill"]
        ):
            get_ops.append(op)
        # SET-related operations
        elif any(
            keyword in op_name_lower
            for keyword in ["set", "lease-set", "add", "cas", "multifill"]
        ):
            set_ops.append(op)
        # Ignore gossip, delete, incr, metaget, etc.
        else:
            other_ops.append(op)

    return {"get": get_ops, "set": set_ops, "other": other_ops}


def calculate_weighted_average(
    operations: List[OperationStats], field_name: str
) -> float:
    """Calculate weighted average based on hits"""
    total_hits = sum(op.hits for op in operations)
    if total_hits == 0:
        return 0.0

    weighted_sum = sum(getattr(op, field_name) * op.hits for op in operations)
    return weighted_sum / total_hits


def calculate_weighted_percentile(
    operations: List[OperationStats], percentile: str
) -> float:
    """Calculate weighted percentile based on hits"""
    field_map = {
        "p50": "reply_wire_value_bytes_p50",
        "p75": "reply_wire_value_bytes_p75",
        "p95": "reply_wire_value_bytes_p95",
        "p99": "reply_wire_value_bytes_p99",
    }

    field_name = field_map[percentile]
    total_hits = sum(op.hits for op in operations)
    if total_hits == 0:
        return 0.0

    weighted_sum = sum(getattr(op, field_name) * op.hits for op in operations)
    return weighted_sum / total_hits


def calculate_set_percentile(
    operations: List[OperationStats], percentile: str
) -> float:
    """Calculate weighted percentile for SET value sizes"""
    field_map = {
        "p50": "request_wire_value_bytes_p50",
        "p75": "request_wire_value_bytes_p75",
        "p95": "request_wire_value_bytes_p95",
        "p99": "request_wire_value_bytes_p99",
    }

    field_name = field_map[percentile]
    total_hits = sum(op.hits for op in operations)
    if total_hits == 0:
        return 0.0

    weighted_sum = sum(getattr(op, field_name) * op.hits for op in operations)
    return weighted_sum / total_hits


def generate_distribution_config(
    operations: List[OperationStats],
) -> DistributionConfig:
    """Generate distribution configuration from parsed operations"""
    categorized = categorize_operations(operations)
    get_ops = categorized["get"]
    set_ops = categorized["set"]

    # Calculate total hits for ratio
    total_get_hits = sum(op.hits for op in get_ops)
    total_set_hits = sum(op.hits for op in set_ops)
    total_hits = total_get_hits + total_set_hits

    get_ratio = total_get_hits / total_hits if total_hits > 0 else 0.9

    # Calculate GET statistics (response sizes)
    get_key_size_avg = calculate_weighted_average(get_ops, "key_size_avg")
    get_response_size_avg = calculate_weighted_average(
        get_ops, "reply_wire_value_bytes_avg"
    )
    get_response_size_p50 = calculate_weighted_percentile(get_ops, "p50")
    get_response_size_p75 = calculate_weighted_percentile(get_ops, "p75")
    get_response_size_p95 = calculate_weighted_percentile(get_ops, "p95")
    get_response_size_p99 = calculate_weighted_percentile(get_ops, "p99")

    # Calculate SET statistics (request value sizes)
    set_key_size_avg = calculate_weighted_average(set_ops, "key_size_avg")
    set_value_size_avg = calculate_weighted_average(
        set_ops, "request_wire_value_bytes_avg"
    )
    set_value_size_p50 = calculate_set_percentile(set_ops, "p50")
    set_value_size_p75 = calculate_set_percentile(set_ops, "p75")
    set_value_size_p95 = calculate_set_percentile(set_ops, "p95")
    set_value_size_p99 = calculate_set_percentile(set_ops, "p99")

    return DistributionConfig(
        get_ratio=get_ratio,
        get_key_size_avg=get_key_size_avg,
        get_response_size_avg=get_response_size_avg,
        get_response_size_p50=get_response_size_p50,
        get_response_size_p75=get_response_size_p75,
        get_response_size_p95=get_response_size_p95,
        get_response_size_p99=get_response_size_p99,
        set_key_size_avg=set_key_size_avg,
        set_value_size_avg=set_value_size_avg,
        set_value_size_p50=set_value_size_p50,
        set_value_size_p75=set_value_size_p75,
        set_value_size_p95=set_value_size_p95,
        set_value_size_p99=set_value_size_p99,
    )


def print_summary(
    config: DistributionConfig, categorized: Dict[str, List[OperationStats]]
) -> None:
    """Print summary of distribution analysis"""
    get_ops = categorized["get"]
    set_ops = categorized["set"]
    other_ops = categorized["other"]

    print("\n=== Traffic Distribution Analysis ===\n")

    print("GET Operations:")
    total_get_hits = sum(op.hits for op in get_ops)
    for op in sorted(get_ops, key=lambda x: x.hits, reverse=True):
        print(
            f"  {op.operation:30s}: {op.hits:15,.0f} hits ({op.hits / total_get_hits * 100:5.2f}%)"
        )

    print(f"\nTotal GET hits: {total_get_hits:,.0f}")

    print("\nSET Operations:")
    total_set_hits = sum(op.hits for op in set_ops)
    for op in sorted(set_ops, key=lambda x: x.hits, reverse=True):
        print(
            f"  {op.operation:30s}: {op.hits:15,.0f} hits ({op.hits / total_set_hits * 100:5.2f}%)"
        )

    print(f"\nTotal SET hits: {total_set_hits:,.0f}")

    print(f"\nIgnored Operations: {len(other_ops)}")
    for op in sorted(other_ops, key=lambda x: x.hits, reverse=True)[:5]:
        print(f"  {op.operation:30s}: {op.hits:15,.0f} hits")

    print("\n=== Distribution Configuration ===\n")
    print(f"GET Ratio: {config.get_ratio:.4f} ({config.get_ratio * 100:.2f}%)")
    print(
        f"SET Ratio: {1 - config.get_ratio:.4f} ({(1 - config.get_ratio) * 100:.2f}%)"
    )
    print(f"\nGET Key Size (avg): {config.get_key_size_avg:.2f} bytes")
    print(
        f"GET Response Size (avg): {config.get_response_size_avg:.2f} bytes"
        f" [p50={config.get_response_size_p50:.0f}, "
        f"p75={config.get_response_size_p75:.0f}, "
        f"p95={config.get_response_size_p95:.0f}, "
        f"p99={config.get_response_size_p99:.0f}]"
    )
    print(f"\nSET Key Size (avg): {config.set_key_size_avg:.2f} bytes")
    print(
        f"SET Value Size (avg): {config.set_value_size_avg:.2f} bytes"
        f" [p50={config.set_value_size_p50:.0f}, "
        f"p75={config.set_value_size_p75:.0f}, "
        f"p95={config.set_value_size_p95:.0f}, "
        f"p99={config.set_value_size_p99:.0f}]"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Parse production ucache traffic distribution"
    )
    parser.add_argument("csv_file", help="Input CSV file with traffic distribution")
    parser.add_argument(
        "--output",
        "-o",
        default="traffic_distribution.json",
        help="Output JSON config file (default: traffic_distribution.json)",
    )
    args = parser.parse_args()

    # Parse CSV
    operations = parse_csv(args.csv_file)
    print(f"Parsed {len(operations)} operations from {args.csv_file}")

    # Categorize operations
    categorized = categorize_operations(operations)

    # Generate configuration
    config = generate_distribution_config(operations)

    # Print summary
    print_summary(config, categorized)

    # Write JSON config
    config_dict = {
        "get_ratio": config.get_ratio,
        "get_key_size_avg": config.get_key_size_avg,
        "get_response_size_avg": config.get_response_size_avg,
        "get_response_size_p50": config.get_response_size_p50,
        "get_response_size_p75": config.get_response_size_p75,
        "get_response_size_p95": config.get_response_size_p95,
        "get_response_size_p99": config.get_response_size_p99,
        "set_key_size_avg": config.set_key_size_avg,
        "set_value_size_avg": config.set_value_size_avg,
        "set_value_size_p50": config.set_value_size_p50,
        "set_value_size_p75": config.set_value_size_p75,
        "set_value_size_p95": config.set_value_size_p95,
        "set_value_size_p99": config.set_value_size_p99,
    }

    with open(args.output, "w") as f:
        json.dump(config_dict, f, indent=2)

    print(f"\n✓ Distribution config written to: {args.output}")
    print(
        f"\nUse with ucache_bench client: --distribution_config={args.output} --use_distribution=true"
    )


if __name__ == "__main__":
    main()
