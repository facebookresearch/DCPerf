#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import json
import sys

import parse_line


def main() -> None:
    if len(sys.argv) != 2:
        print("Usage: convert.py <benchmark_name>")
        sys.exit(1)

    benchmark_name = sys.argv[1]
    input_file_name = "out_" + benchmark_name + ".txt"
    sum_c = {}

    parser = parse_line.get_parser(benchmark_name)
    with open(input_file_name) as f:
        parser(f, sum_c)

    out_file_name = "out_" + benchmark_name + ".json"
    with open(out_file_name, "w") as f:
        json.dump(sum_c, f, indent=4, sort_keys=True)


if __name__ == "__main__":
    main()
