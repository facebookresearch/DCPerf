#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import glob
import json
import sys

import parse_line


def main() -> None:
    if len(sys.argv) != 2:
        print("Usage: aggregate_result.py <benchmark_name>")
        sys.exit(1)

    benchmark_name = sys.argv[1]
    sum_c = {}
    parser = parse_line.get_parser(benchmark_name)

    for n in glob.glob("output_file_*"):
        with open(n) as f:
            parser(f, sum_c)

    out_file_name = "out_" + benchmark_name + ".json"
    with open(out_file_name, "w") as f:
        json.dump(sum_c, f, indent=4, sort_keys=True)


if __name__ == "__main__":
    main()
