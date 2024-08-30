#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import re

from benchpress.lib.parser import Parser


class WDLParser(Parser):

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        benchmarks = []
        for line in stdout:
            if re.search("benchmark results", line):
                benchmarks = line.split(":")[1].split()
                break

        for benchmark in benchmarks:
            out_file = "benchmarks/wdl_bench/out_" + benchmark + ".json"

            with open(out_file, "r") as out_f:
                out = json.load(out_f)
                for k, v in out.items():
                    metrics[benchmark + k] = v

        if len(metrics.keys()) >= 20:
            metrics = {}
            metrics["results in each out_benchmark_name.json file"] = benchmarks

        return metrics
