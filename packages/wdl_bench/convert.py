#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import sys

import parse_line

sum_c = {}

input_file_name = "out_" + sys.argv[1] + ".txt"


with open(input_file_name) as f:
    if sys.argv[1] == "concurrency_concurrent_hash_map_bench":
        parse_line.parse_line_chm(f, sum_c)
    elif sys.argv[1] == "lzbench":
        parse_line.parse_line_lzbench(f, sum_c)
    elif sys.argv[1] == "openssl":
        parse_line.parse_line_openssl(f, sum_c)
    elif sys.argv[1] == "vdso_bench":
        parse_line.parse_line_vdso_bench(f, sum_c)
    elif sys.argv[1] == "libaegis_benchmark":
        parse_line.parse_line_libaegis_benchmark(f, sum_c)
    elif sys.argv[1] == "xxhash_benchmark":
        parse_line.parse_line_xxhash_benchmark(f, sum_c)
    elif sys.argv[1] == "container_hash_maps_bench":
        parse_line.parse_line_container_hash_maps_bench(f, sum_c)
    elif sys.argv[1] == "erasure_code_perf":
        parse_line.parse_line_erasure_code_perf(f, sum_c)
    else:
        parse_line.parse_line(f, sum_c)

out_file_name = "out_" + sys.argv[1] + ".json"
with open(out_file_name, "w") as f:
    json.dump(sum_c, f, indent=4, sort_keys=True)
