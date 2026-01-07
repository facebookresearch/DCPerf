#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Compare benchmark results from two JSON files and generate a ratio comparison.

Usage:
    python compare_results.py <file1.json> <file2.json>

The script compares the results from file1 and file2 for the same microbenchmarks,
and outputs the ratios (file1/file2) sorted from high to low.

Special handling is provided for:
- xxhash_benchmark
- concurrency_concurrent_hash_map_bench
- benchsleef (benchsleef128, benchsleef256, benchsleef512)
- bench-memcmp
"""

import json
import os
import sys
from typing import Any


def detect_benchmark_type(filename: str) -> str:
    """
    Detect the benchmark type from the filename.

    Args:
        filename: The name of the benchmark file

    Returns:
        The detected benchmark type
    """
    basename = os.path.basename(filename).lower()

    if "xxhash" in basename:
        return "xxhash_benchmark"
    elif "concurrent_hash_map" in basename:
        return "concurrency_concurrent_hash_map_bench"
    elif "benchsleef" in basename:
        return "benchsleef"
    elif "memcmp" in basename:
        return "bench-memcmp"
    else:
        return "default"


def extract_xxhash_results(data: dict[str, Any]) -> dict[str, float]:
    """
    Extract results from xxhash_benchmark format.

    Args:
        data: The JSON data containing benchmark results

    Returns:
        Dictionary mapping benchmark name to result value
    """
    results = {}
    if "large_inputs" in data and "xxh3" in data["large_inputs"]:
        for key, value in data["large_inputs"]["xxh3"].items():
            results[f"large_inputs/xxh3/{key}"] = float(value)
    return results


def extract_memcmp_timings_for_compare(
    data: dict[str, Any],
) -> dict[str, float]:
    """
    Extract timings from bench-memcmp format for comparison.

    Only includes results where align1 == align2 == result == 0.

    Args:
        data: The JSON data containing benchmark results

    Returns:
        Dictionary mapping benchmark name to timing value
    """
    results = {}

    for func_name, func_data in data.get("functions", {}).items():
        ifuncs = func_data.get("ifuncs", [])

        for result in func_data.get("results", []):
            if (
                result.get("align1") == 0
                and result.get("align2") == 0
                and result.get("result") == 0
            ):
                length = result.get("length")
                timings = result.get("timings", [])

                for ifunc_index, ifunc_name in enumerate(ifuncs):
                    if ifunc_index < len(timings):
                        bench_name = f"{func_name}/{ifunc_name}/len={length}"
                        results[bench_name] = timings[ifunc_index]

    return results


def extract_sleef_results(data: dict[str, Any]) -> dict[str, float]:
    """
    Extract results from benchsleef format.

    Args:
        data: The JSON data containing benchmark results

    Returns:
        Dictionary mapping benchmark name to NSperEl value
    """
    results = {}
    for benchmark in data.get("benchmarks", []):
        name = benchmark.get("name", "")
        if "NSperEl" in benchmark:
            results[name] = float(benchmark["NSperEl"])
    return results


def extract_concurrent_hash_map_results(data: dict[str, Any]) -> dict[str, float]:
    """
    Extract results from concurrency_concurrent_hash_map_bench format.

    Note: For this benchmark, higher values are better (throughput), so
    the ratio calculation is inverted (file2/file1 instead of file1/file2).

    Args:
        data: The JSON data containing benchmark results

    Returns:
        Dictionary mapping benchmark name to result value
    """
    results = {}
    for key, value in data.items():
        results[key] = float(value)
    return results


def extract_default_results(data: dict[str, Any]) -> dict[str, float]:
    """
    Extract results from default benchmark format (flat key-value pairs).

    Args:
        data: The JSON data containing benchmark results

    Returns:
        Dictionary mapping benchmark name to result value
    """
    results = {}
    for key, value in data.items():
        if isinstance(value, (int, float)):
            results[key] = float(value)
    return results


def compare_results(
    data1: dict[str, Any],
    data2: dict[str, Any],
    benchmark_type: str,
) -> list[tuple[str, float, float, float]]:
    """
    Compare results from two benchmark data sets.

    Args:
        data1: The first benchmark data set
        data2: The second benchmark data set
        benchmark_type: The type of benchmark being compared

    Returns:
        List of tuples (benchmark_name, value1, value2, ratio) sorted by ratio (high to low)
    """
    if benchmark_type == "xxhash_benchmark":
        results1 = extract_xxhash_results(data1)
        results2 = extract_xxhash_results(data2)
    elif benchmark_type == "bench-memcmp":
        results1 = extract_memcmp_timings_for_compare(data1)
        results2 = extract_memcmp_timings_for_compare(data2)
    elif benchmark_type == "benchsleef":
        results1 = extract_sleef_results(data1)
        results2 = extract_sleef_results(data2)
    elif benchmark_type == "concurrency_concurrent_hash_map_bench":
        results1 = extract_concurrent_hash_map_results(data1)
        results2 = extract_concurrent_hash_map_results(data2)
    else:
        results1 = extract_default_results(data1)
        results2 = extract_default_results(data2)

    comparisons = []
    common_keys = set(results1.keys()) & set(results2.keys())

    for key in common_keys:
        val1 = results1[key]
        val2 = results2[key]

        if val2 != 0:
            # For concurrency_concurrent_hash_map_bench, higher is better (throughput)
            # so we invert the ratio to show speedup consistently
            if benchmark_type == "concurrency_concurrent_hash_map_bench":
                ratio = val1 / val2
            else:
                # For timing-based benchmarks, lower is better
                # ratio > 1 means file2 is faster
                ratio = val1 / val2
            comparisons.append((key, val1, val2, ratio))

    # Sort by ratio from high to low
    comparisons.sort(key=lambda x: x[3], reverse=True)

    return comparisons


def generate_output_filename(file1: str, file2: str) -> str:
    """
    Generate output filename based on input filenames.

    Args:
        file1: First input filename
        file2: Second input filename

    Returns:
        Output filename
    """
    base1 = os.path.splitext(os.path.basename(file1))[0]
    base2 = os.path.splitext(os.path.basename(file2))[0]
    return f"comparison_{base1}_vs_{base2}.txt"


def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: compare_results.py <file1.json> <file2.json>")
        print("")
        print("Compares benchmark results from two JSON files and generates")
        print("a ratio comparison (file1/file2) sorted from high to low.")
        sys.exit(1)

    file1 = sys.argv[1]
    file2 = sys.argv[2]

    # Load the data
    try:
        with open(file1) as f1:
            data1 = json.load(f1)
    except FileNotFoundError:
        print(f"Error: File not found: {file1}")
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {file1}: {e}")
        sys.exit(1)

    try:
        with open(file2) as f2:
            data2 = json.load(f2)
    except FileNotFoundError:
        print(f"Error: File not found: {file2}")
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in {file2}: {e}")
        sys.exit(1)

    # Detect benchmark type from filename
    benchmark_type = detect_benchmark_type(file1)

    # Compare the results
    comparisons = compare_results(data1, data2, benchmark_type)

    if not comparisons:
        print("No common benchmarks found between the two files.")
        sys.exit(1)

    # Generate output filename
    output_file = generate_output_filename(file1, file2)

    # Write output
    with open(output_file, "w") as f:
        f.write(
            f"# Comparison: {os.path.basename(file1)} vs {os.path.basename(file2)}\n"
        )
        f.write(f"# Benchmark type: {benchmark_type}\n")
        f.write(f"# Ratio = file1 / file2\n")
        f.write(f"# Sorted from high to low ratio\n")
        f.write("#\n")
        f.write(f"# {'Benchmark':<80} {'File1':>15} {'File2':>15} {'Ratio':>10}\n")
        f.write(f"# {'-'*80} {'-'*15} {'-'*15} {'-'*10}\n")

        for name, val1, val2, ratio in comparisons:
            f.write(f"{name:<80} {val1:>15.6f} {val2:>15.6f} {ratio:>10.4f}\n")

        f.write(f"\n# Total benchmarks compared: {len(comparisons)}\n")

    print(f"Comparison results written to: {output_file}")
    print(f"Benchmark type detected: {benchmark_type}")
    print(f"Total benchmarks compared: {len(comparisons)}")

    # Also print summary to stdout
    print("\nTop 10 highest ratios:")
    for name, _, _, ratio in comparisons[:10]:
        print(f"  {name}: {ratio:.4f}")

    print("\nTop 10 lowest ratios:")
    for name, _, _, ratio in comparisons[-10:]:
        print(f"  {name}: {ratio:.4f}")


if __name__ == "__main__":
    main()
