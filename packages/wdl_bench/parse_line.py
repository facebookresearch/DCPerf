# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import re


def parse_line_chm(f, sum_c):
    thread_count = 0
    for line in f:
        if re.search("threads", line):
            thread_count = int(line.split()[1])
        elif re.search("CHM", line):
            elements = line.split()
            idx_name = 0
            for i in range(len(elements)):
                if re.search("(item)|(empty)", elements[i]):
                    idx_name = i

            bench_name = (
                str(thread_count) + "threads " + " ".join(elements[: idx_name + 1])
            )
            bench_name = bench_name + ": ns"
            # max_latency = "".join(elements[idx_name + 1 : idx_name + 3])
            avg_latency = "".join(elements[idx_name + 3 : idx_name + 5])
            # min_latency = "".join(elements[idx_name + 5 : idx_name + 7])

            if avg_latency[-2] == "u":
                avg_latency = int(avg_latency[:-2]) * 1000
            elif avg_latency[-2] == "m":
                avg_latency = int(avg_latency[:-2]) * 1000 * 1000
            else:
                avg_latency = int(avg_latency[:-2])
            sum_c[bench_name] = avg_latency


def find_idx_time(elements):
    has_relative = False
    idx_time = 0
    for i in range(len(elements)):
        if re.search("%", elements[i]):
            has_relative = True
        if re.search("([0-9](n|m|u|f|p)s)|([0-9]s)", elements[i]):
            idx_time = i
            break

    return has_relative, idx_time


def parse_line(f, sum_c):
    for line in f:
        # capture the time unit here (ns, us, ms, s, ps, fs)
        if re.search("([0-9](n|m|u|f|p)s)|([0-9]s)", line):
            elements = line.split()
            has_relative, idx_time = find_idx_time(elements)
            throughput = elements[idx_time + 1]
            bench_name = None
            if has_relative:
                bench_name = " ".join(elements[: idx_time - 1])
            else:
                bench_name = " ".join(elements[:idx_time])
            bench_name = bench_name + ": iters/s"
            if throughput[-1] == "K":
                throughput = float(throughput[:-1]) * 1000
            elif throughput[-1] == "M":
                throughput = float(throughput[:-1]) * 1000 * 1000
            elif throughput[-1] == "G":
                throughput = float(throughput[:-1]) * 1000 * 1000 * 1000
            elif throughput[-1] == "T":
                throughput = float(throughput[:-1]) * 1000 * 1000 * 1000 * 1000
            elif throughput[-1] == "m":
                throughput = float(throughput[:-1]) / 1000
            elif throughput == "Infinity":
                throughput = float("inf")
            else:
                throughput = float(throughput)

            if bench_name not in sum_c:
                sum_c[bench_name] = 0
            sum_c[bench_name] += throughput


def parse_line_lzbench(f, sum_c):
    for line in f:
        if re.search("datasets", line):
            elements = line.split()
            idx_time = 0
            for i in range(len(elements)):
                if re.search("MB", elements[i]):
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
        elif "," in line and current_section:
            parts = [p.strip() for p in line.split(",")]
            if len(parts) > 1:
                hash_name = parts[0]
                values = [int(v) for v in parts[1:] if v]

                # Create input size keys based on section and position
                data = {}
                for i, value in enumerate(values):
                    if current_section == "large_inputs":
                        # log9 to log27 (512 bytes to 128 MB)
                        input_size = f"log{9+i}"
                    else:
                        # 1 to N bytes
                        input_size = f"{i+1}_bytes"
                    data[input_size] = value

                sum_c[current_section][hash_name] = data


def parse_line_container_hash_maps_bench(f, sum_c):
    data = json.load(f)
    for k, v in data.items():
        if re.search("^(Find)|(Insert)|(InsertSqBr)|(Erase)|(Iter)", k):
            sum_c[k] = v


def parse_line_erasure_code_perf(f, sum_c):
    for line in f:
        elements = line.split()
        if re.search("warm", elements[0]):
            name = elements[0]
            value = float(elements[-2])
            sum_c[name + ": MB/s"] = value
