#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import sys
from typing import Any

import numpy as np

sum_c = {}

if len(sys.argv) != 2:
    print("scoring.py benchmark_name")
    sys.exit(-1)


def weighted_geomean(values: list[float], weights: list[float]) -> float:
    return np.exp(np.average(np.log(np.array(values)), weights=weights))


def geomean(values: list[float]) -> float:
    return np.exp(np.mean(np.log(np.array(values))))


def max(values: list[float]) -> float:
    return float(np.max(np.array(values)))


benchmark_name = sys.argv[1]

input_file_name = "out_" + benchmark_name + ".json"

baseline_name = "baseline_results/baseline_" + benchmark_name + ".json"

# For sleef, we use the benchsleef256 results collected on
# Intel Cooperlake as the baseline since AVX512 may not be enabled
# on many prod servers.
if benchmark_name.startswith("benchsleef"):
    baseline_name = "baseline_results/baseline_benchsleef256.json"


def generate_sleef_benchmark_name(
    math_function_name: str,
    vector_bit_length: int,
    value_range: str,
    is_sve: bool = False,
) -> str:
    if is_sve:
        element_count = "x"
        vector_bit_length = ""
        precision = "_u10sve"
        vector_name = "sve"
    else:
        element_count = str(int(vector_bit_length / 32))
        vector_bit_length = str(vector_bit_length)
        precision = "_u10"
        vector_name = "vector"

    benchmark_name = f"MB_Sleef_{math_function_name}{element_count}{precision}_{vector_name}f{vector_bit_length}_{value_range}"
    return benchmark_name


def extract_gbench_metric(
    data: dict[str, Any],
    benchmark_names: list[str],
    metric_key: str,
    match_prefix: bool = False,
) -> list[float]:
    results: list[float] = []
    for name in benchmark_names:
        for b in data["benchmarks"]:
            if b["name"] == name:
                results.append(float(b[metric_key]))
            elif match_prefix and b["name"].startswith(name):
                results.append(float(b[metric_key]))
    return results


def calculate_sleef_score(sum_baseline: dict[str, Any], sum_c: dict[str, Any]) -> float:
    # Function name, value range
    math_functions = [["expf", "-700_700"], ["logf", "0_1e+38"]]
    math_function_weights = [80, 20]

    baseline_names = []
    for math_function in math_functions:
        baseline_names.append(
            generate_sleef_benchmark_name(math_function[0], 512, math_function[1])
        )

    baseline_time = weighted_geomean(
        extract_gbench_metric(sum_baseline, baseline_names, "NSperEl"),
        math_function_weights,
    )

    sve_variant_names = []
    for math_function in math_functions:
        sve_variant_names.append(
            generate_sleef_benchmark_name(math_function[0], "x", math_function[1], True)
        )

    sve_ns_per_elem = extract_gbench_metric(sum_c, sve_variant_names, "NSperEl")
    if len(sve_ns_per_elem) > 0:
        sve_variant_time = weighted_geomean(sve_ns_per_elem, math_function_weights)
    else:
        sve_variant_time = float("inf")

    fixed_vl_variant_names = []
    for math_function in math_functions:
        fixed_vl_variant_names.append(
            generate_sleef_benchmark_name(
                math_function[0],
                int(benchmark_name[-3:]),
                math_function[1],
            )
        )
    fixed_vl_variant_time = weighted_geomean(
        extract_gbench_metric(sum_c, fixed_vl_variant_names, "NSperEl"),
        math_function_weights,
    )

    score = baseline_time / min(fixed_vl_variant_time, sve_variant_time)

    return score


def extract_memcmp_timings(
    data: dict[str, Any], ifunc_name: str
) -> list[tuple[int, float]]:
    """
    Extract timings for a given ifunc name, sorted by length.

    Only includes results where align1 == align2 == result == 0.

    Args:
        data: The JSON data containing benchmark results
        ifunc_name: The name of the ifunc to extract timings for

    Returns:
        List of (length, timing) tuples sorted by length
    """
    results: list[tuple[int, float]] = []

    for func_data in data.get("functions", {}).values():
        ifuncs = func_data.get("ifuncs", [])

        if ifunc_name not in ifuncs:
            continue

        ifunc_index = ifuncs.index(ifunc_name)

        for result in func_data.get("results", []):
            if (
                result.get("align1") == 0
                and result.get("align2") == 0
                and result.get("result") == 0
            ):
                length = result.get("length")
                timings = result.get("timings", [])
                if ifunc_index < len(timings):
                    results.append((length, timings[ifunc_index]))

    results.sort(key=lambda x: x[0])
    return results


def calculate_memcmp_geomean_by_range(
    data: dict[str, Any],
    ifunc_name: str,
) -> float:
    """
    Calculate geometric mean of timings for different length ranges.

    Only includes results where align1 == align2 == result == 0.

    Ranges:
        - small:  [0, 16)      (0 <= length < 16)
        - medium: [16, 64)     (16 <= length < 64)
        - large:  [64, inf)    (length >= 64)

    Args:
        data: The JSON data containing benchmark results
        ifunc_name: The name of the ifunc to extract timings for

    Returns:
        Geometric mean of timings.
        0 if no timings are available.
    """
    timings = extract_memcmp_timings(data, ifunc_name)

    if len(timings) == 0:
        return 0.0

    small: list[float] = []
    medium: list[float] = []
    large: list[float] = []

    for length, timing in timings:
        if 0 <= length < 16:
            small.append(timing)
        elif 16 <= length < 64:
            medium.append(timing)
        elif length >= 64:
            large.append(timing)

    range_geomean = [geomean(small), geomean(medium), geomean(large)]
    weights = [56, 37, 7]  # extracted from a variety of real workloads
    return weighted_geomean(range_geomean, weights)


def get_memcmp_variant_names(data: dict[str, Any]) -> list[str]:
    """
    Get list of all ifunc names from the benchmark data.

    Args:
        data: The JSON data containing benchmark results

    Returns:
        List of ifunc names
    """
    for func_data in data.get("functions", {}).values():
        return func_data.get("ifuncs", [])
    return []


def calculate_memcmp_score(
    sum_baseline: dict[str, Any], sum_c: dict[str, Any]
) -> float:
    # Use AVX512 memcmp as the baseline since it is the fastest variant
    # in baseline.
    baseline_time = calculate_memcmp_geomean_by_range(
        sum_baseline, "__memcmp_evex_movbe"
    )
    min_time = float("inf")
    for name in get_memcmp_variant_names(sum_c):
        variant_time = calculate_memcmp_geomean_by_range(sum_c, name)
        min_time = min(min_time, variant_time)

    return baseline_time / min_time


def calculate_stdcpp_score(
    sum_baseline: dict[str, Any], sum_c: dict[str, Any]
) -> float:
    """
    Calculate stdcpp_bench score using weighted geometric mean.

    Args:
        sum_baseline: Baseline benchmark results
        sum_c: Current benchmark results

    Returns:
        Score as ratio of baseline to current performance
    """
    benchmark_names = ["BM_SharedPtr_IncDec", "BM_WeakPtr_IncDec"]
    weights = [0.9, 0.1]

    baseline_times = extract_gbench_metric(sum_baseline, benchmark_names, "cpu_time")
    current_times = extract_gbench_metric(sum_c, benchmark_names, "cpu_time")

    baseline_geomean = weighted_geomean(baseline_times, weights)
    current_geomean = weighted_geomean(current_times, weights)

    return baseline_geomean / current_geomean


def calculate_gemm_score(sum_baseline: dict[str, Any], sum_c: dict[str, Any]) -> float:
    """
    Calculate gemm_bench score based on peak flops.

    Args:
        sum_baseline: Baseline benchmark results
        sum_c: Current benchmark results

    Returns:
        Score as ratio of current peak to baseline peak flops
    """

    baseline_peak_ops = [
        max(extract_gbench_metric(sum_baseline, ["BM_SGEMM"], "FLOPS", True)),
        max(extract_gbench_metric(sum_baseline, ["BM_BF16GEMM"], "FLOPS", True)),
        max(extract_gbench_metric(sum_baseline, ["BM_I8GEMM"], "TOPS", True)),
    ]
    current_peak_ops = [
        max(extract_gbench_metric(sum_c, ["BM_SGEMM"], "FLOPS", True)),
        max(extract_gbench_metric(sum_c, ["BM_BF16GEMM"], "FLOPS", True)),
        max(extract_gbench_metric(sum_c, ["BM_I8GEMM"], "TOPS", True)),
    ]

    try:
        # Special case for AMD CPUs where BM_HGEMM is not supported, in which case
        # an exception is thrown and we don't add BM_HGEMM to the list of peak
        # baseline ops.
        current_peak_ops.append(
            max(extract_gbench_metric(sum_c, ["BM_HGEMM"], "FLOPS", True))
        )
        baseline_peak_ops.append(
            max(extract_gbench_metric(sum_baseline, ["BM_HGEMM"], "FLOPS", True))
        )
    except Exception:
        pass

    return geomean(current_peak_ops) / geomean(baseline_peak_ops)


with open(input_file_name) as f:
    with open(baseline_name) as f_baseline:
        sum_c = json.load(f)
        sum_baseline = json.load(f_baseline)

        scores = []
        score = 0

        if benchmark_name == "memcpy_benchmark":
            for low, high in [
                ("0", "7"),
                ("8", "16"),
                ("16", "32"),
                ("32", "256"),
                ("256", "1024"),
                ("1024", "8192"),
                ("8192", "32768"),
            ]:
                scores.append(
                    (
                        sum_c["%bench(" + low + "_to_" + high + "_COLD_folly)"]
                        / sum_baseline["%bench(" + low + "_to_" + high + "_COLD_folly)"]
                        + sum_c["%bench(" + low + "_to_" + high + "_HOT_folly)"]
                        / sum_baseline["%bench(" + low + "_to_" + high + "_HOT_folly)"]
                    )
                    / 2
                )
            weights = np.array([1, 1.38, 1.02, 0.61, 0.33, 0.05, 0.01])
            score = weighted_geomean(scores, weights)
        elif benchmark_name == "memset_benchmark":
            size = 1
            while size <= 32768:
                scores.append(
                    sum_c["folly::__folly_memset: size=" + str(size)]
                    / sum_baseline["folly::__folly_memset: size=" + str(size)]
                )
                size *= 2
            weights = np.array(
                [
                    1,
                    6.38,
                    13.41,
                    64.81,
                    52.82,
                    12.3,
                    13.48,
                    11.8,
                    4.79,
                    4.8,
                    4.72,
                    2.1,
                    0.85,
                    0.45,
                    0.1,
                    0.06,
                ]
            )
            score = weighted_geomean(scores, weights)
        elif benchmark_name == "xxhash_benchmark":
            res_large = sum_c["large_inputs"]["xxh3"]
            res_baseline = sum_baseline["large_inputs"]["xxh3"]
            for key in res_large:
                scores.append(res_large[key] / res_baseline[key])
            score = geomean(np.array(scores))
        elif benchmark_name == "concurrency_concurrent_hash_map_bench":
            for key in sum_baseline:
                if key in sum_c:
                    scores.append(sum_baseline[key] / sum_c[key])
            score = geomean(np.array(scores))
        elif benchmark_name.startswith("benchsleef"):
            score = calculate_sleef_score(sum_baseline, sum_c)
        elif benchmark_name == "bench-memcmp":
            score = calculate_memcmp_score(sum_baseline, sum_c)
        elif benchmark_name == "stdcpp_bench":
            score = calculate_stdcpp_score(sum_baseline, sum_c)
        elif benchmark_name == "gemm_bench":
            score = calculate_gemm_score(sum_baseline, sum_c)
        else:
            for key in sum_baseline:
                if key in sum_c:
                    scores.append(sum_c[key] / sum_baseline[key])
            score = geomean(np.array(scores))

print(benchmark_name + f" score: {score:.2f}")
