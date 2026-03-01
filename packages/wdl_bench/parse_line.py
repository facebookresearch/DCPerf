# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import re

_TIME_UNIT_RE = re.compile(r"([0-9](n|m|u|f|p)s)|([0-9]s)")
_HASH_MAP_OPS_RE = re.compile(r"^(Find)|(Insert)|(InsertSqBr)|(Erase)|(Iter)")

_THROUGHPUT_SUFFIX = {
    "K": 1e3,
    "M": 1e6,
    "G": 1e9,
    "T": 1e12,
    "m": 1e-3,
}

_LATENCY_SUFFIX = {
    "u": 1000,
    "m": 1000 * 1000,
}


def _parse_throughput(raw: str) -> float:
    if raw == "Infinity":
        return float("inf")
    if raw[-1] in _THROUGHPUT_SUFFIX:
        return float(raw[:-1]) * _THROUGHPUT_SUFFIX[raw[-1]]
    return float(raw)


def _parse_latency_ns(raw: str) -> int:
    suffix = raw[-2]
    value = int(raw[:-2])
    if suffix in _LATENCY_SUFFIX:
        return value * _LATENCY_SUFFIX[suffix]
    return value


def parse_line_chm(f, sum_c):
    thread_count = 0
    for line in f:
        if "threads" in line:
            thread_count = int(line.split()[1])
        elif "CHM" in line:
            elements = line.split()
            idx_name = 0
            for i, elem in enumerate(elements):
                if re.search("(item)|(empty)", elem):
                    idx_name = i

            bench_name = (
                str(thread_count) + "threads " + " ".join(elements[: idx_name + 1])
            )
            bench_name = bench_name + ": ns"
            avg_latency = "".join(elements[idx_name + 3 : idx_name + 5])
            sum_c[bench_name] = _parse_latency_ns(avg_latency)


def find_idx_time(elements):
    has_relative = False
    idx_time = 0
    for i, elem in enumerate(elements):
        if "%" in elem:
            has_relative = True
        if _TIME_UNIT_RE.search(elem):
            idx_time = i
            break

    return has_relative, idx_time


def parse_line(f, sum_c):
    for line in f:
        # capture the time unit here (ns, us, ms, s, ps, fs)
        if _TIME_UNIT_RE.search(line):
            elements = line.split()
            has_relative, idx_time = find_idx_time(elements)
            throughput = elements[idx_time + 1]
            if has_relative:
                bench_name = " ".join(elements[: idx_time - 1])
            else:
                bench_name = " ".join(elements[:idx_time])
            bench_name = bench_name + ": iters/s"
            throughput = _parse_throughput(throughput)

            if bench_name not in sum_c:
                sum_c[bench_name] = 0
            sum_c[bench_name] += throughput


def parse_line_lzbench(f, sum_c):
    for line in f:
        if "datasets" in line:
            elements = line.split()
            idx_time = 0
            for i, elem in enumerate(elements):
                if "MB" in elem:
                    idx_time = i
                    break
            throughput_decomp = float(elements[idx_time + 1])
            throughput_comp = float(elements[idx_time - 1])
            bench_name_decomp = (
                " ".join(elements[: idx_time - 1]) + " decompression: MB/s"
            )
            bench_name_comp = " ".join(elements[: idx_time - 1]) + " compression: MB/s"

            if bench_name_decomp not in sum_c:
                sum_c[bench_name_decomp] = 0
            if bench_name_comp not in sum_c:
                sum_c[bench_name_comp] = 0

            sum_c[bench_name_decomp] += throughput_decomp
            sum_c[bench_name_comp] += throughput_comp


def parse_line_openssl(f, sum_c):
    last_line = None
    for line in f:
        last_line = line

    if last_line is None:
        return

    elements = last_line.split()
    name = elements[0]

    sum_c[name + " 16B: KB/s"] = float(elements[1][:-1])
    sum_c[name + " 64B: KB/s"] = float(elements[2][:-1])
    sum_c[name + " 256B: KB/s"] = float(elements[3][:-1])
    sum_c[name + " 1KB: KB/s"] = float(elements[4][:-1])
    sum_c[name + " 8KB: KB/s"] = float(elements[5][:-1])
    sum_c[name + " 16KB: KB/s"] = float(elements[6][:-1])


def parse_line_vdso_bench(f, sum_c):
    for line in f:
        elements = line.split()
        if re.search("Number", elements[0]):
            name = elements[4]
            value = float(elements[7])
            sum_c[name + ": M/s"] = value


def parse_line_libaegis_benchmark(f, sum_c):
    for line in f:
        elements = line.split()
        if re.search("128L", elements[0]):
            name = " ".join(elements[:-2])
            value = float(elements[-2])
            sum_c[name + ": Mb/s"] = value


def parse_line_xxhash_benchmark(f, sum_c):
    current_section = None
    for line in f:
        line = line.strip()
        if not line:
            continue

        # Detect section headers
        if "benchmarking large inputs" in line.lower():
            current_section = "large_inputs"
            sum_c[current_section] = {}
        elif "throughput small inputs of fixed size" in line.lower():
            current_section = "throughput_small_fixed"
            sum_c[current_section] = {}
        elif "benchmarking random size inputs" in line.lower():
            current_section = "random_size_inputs"
            sum_c[current_section] = {}
        elif "latency for small inputs of fixed size" in line.lower():
            current_section = "latency_small_fixed"
            sum_c[current_section] = {}
        elif "latency for small inputs of random size" in line.lower():
            current_section = "latency_small_random"
            sum_c[current_section] = {}
        # Parse data lines (format: "xxh3   , value1, value2, ...")
        elif "," in line and current_section is not None:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) > 1:
                hash_name = parts[0]
                values = [int(v) for v in parts[1:] if v]

                # Create input size keys based on section and position
                data = {}
                for i, value in enumerate(values):
                    if current_section == "large_inputs":
                        # log9 to log27 (512 bytes to 128 MB)
                        input_size = f"log{9 + i}"
                    else:
                        # 1 to N bytes
                        input_size = f"{i + 1}_bytes"
                    data[input_size] = value

                sum_c[current_section][hash_name] = data


def parse_line_container_hash_maps_bench(f, sum_c):
    data = json.load(f)
    for k, v in data.items():
        if _HASH_MAP_OPS_RE.search(k):
            sum_c[k] = v


def parse_line_erasure_code_perf(f, sum_c):
    for line in f:
        elements = line.split()
        if re.search("warm", elements[0]):
            name = elements[0]
            value = float(elements[-2])
            sum_c[name + ": MB/s"] = value


# Registry mapping benchmark names to parser functions.
# The default parser (parse_line) is used for benchmarks not listed here.
PARSER_REGISTRY = {
    "concurrency_concurrent_hash_map_bench": parse_line_chm,
    "lzbench": parse_line_lzbench,
    "openssl": parse_line_openssl,
    "vdso_bench": parse_line_vdso_bench,
    "libaegis_benchmark": parse_line_libaegis_benchmark,
    "xxhash_benchmark": parse_line_xxhash_benchmark,
    "container_hash_maps_bench": parse_line_container_hash_maps_bench,
    "erasure_code_perf": parse_line_erasure_code_perf,
}


def get_parser(benchmark_name):
    """Return the parser function for the given benchmark name."""
    return PARSER_REGISTRY.get(benchmark_name, parse_line)
