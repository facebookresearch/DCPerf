#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import sys

import numpy as np


def geo_mean(iterable):
    a = np.array(iterable)
    return a.prod() ** (1.0 / len(a))


sum_c = {}

input_file_name = "out_" + sys.argv[1] + ".json"

baseline_name = "baseline_results/baseline_" + sys.argv[1] + ".json"

with open(input_file_name) as f:
    with open(baseline_name) as f_baseline:
        sum_c = json.load(f)
        sum_baseline = json.load(f_baseline)

        scores = []
        score = 0

        if sys.argv[1] == "memcpy_benchmark":
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
            score = np.exp(np.average(np.log(np.array(scores)), weights=weights))
        elif sys.argv[1] == "memset_benchmark":
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
            score = np.exp(np.average(np.log(np.array(scores)), weights=weights))
        elif sys.argv[1] == "xxhash_benchmark":
            res_large = sum_c["large_inputs"]["xxh3"]
            res_baseline = sum_baseline["large_inputs"]["xxh3"]
            for key in res_large:
                scores.append(res_large[key] / res_baseline[key])
            score = geo_mean(np.array(scores))
        elif sys.argv[1] == "concurrency_concurrent_hash_map_bench":
            for key in sum_baseline:
                if key in sum_c:
                    scores.append(sum_baseline[key] / sum_c[key])
            score = geo_mean(np.array(scores))

        else:
            for key in sum_baseline:
                if key in sum_c:
                    scores.append(sum_c[key] / sum_baseline[key])
            score = geo_mean(np.array(scores))

print(sys.argv[1] + f" score: {score:.2f}")
