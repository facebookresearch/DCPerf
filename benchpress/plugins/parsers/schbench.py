#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import re
from typing import Tuple

from benchpress.lib.parser import Parser

REGEX = r"\s*\*?\d\d\.\dth:\s(\d+)\s"

logger = logging.getLogger(__name__)


class SchbenchParser(Parser):
    """
    Example output:

    Wakeup Latencies percentiles (usec) runtime 20 (s) (146347 total samples)
            50.0th: 16         (41202 samples)
            90.0th: 143        (57173 samples)
            * 99.0th: 3060       (13036 samples)
            99.9th: 3996       (1319 samples)
            min=1, max=18437
    Request Latencies percentiles (usec) runtime 20 (s) (146489 total samples)
            50.0th: 9168       (45298 samples)
            90.0th: 11056      (57303 samples)
            * 99.0th: 24288      (13117 samples)
            99.9th: 37952      (1315 samples)
            min=4790, max=87041
    RPS percentiles (requests) runtime 20 (s) (21 total samples)
            20.0th: 7032       (5 samples)
            * 50.0th: 7384       (8 samples)
            90.0th: 7656       (6 samples)
            min=6707, max=7706
    average rps: 7324.45
    """

    @staticmethod
    def parse_latency(line: str) -> Tuple[str, str]:
        line_clean = line.replace("*", "").strip()
        toks = line_clean.split(":")
        if len(toks) == 2 and line.endswith("samples)"):
            pctl = toks[0].replace("th", "")
            val = toks[1].strip().split(" ")[0]

            return pctl, val

        return "", ""

    def parse(self, stdout, stderr, returncode):
        stdout = stderr  # schbench writes it output on stderr
        metrics = {}

        latency_percs = ["p50", "p75", "p90", "p95", "p99", "p99_5", "p99_9"]

        line_nums = {}
        # Get the line number of the three different metrics reported by the benchmark
        for i, l in enumerate(stdout):
            if "Wakeup Latencies percentiles" in l:
                line_nums["wakeup"] = i
            elif "Request Latencies percentiles" in l:
                line_nums["request"] = i
            elif "RPS percentiles" in l:
                line_nums["rps"] = i
            elif "average rps" in l:
                metrics["rps_average"] = l.split(":")[1].strip()

        # Parse each of the sections
        for metric_type, line_num in line_nums.items():
            for _, line in zip(latency_percs, stdout[line_num + 1 :]):
                pctl, val = SchbenchParser.parse_latency(line)
                if pctl != "":
                    suffix = "_usecs"
                    if "rps" in metric_type:
                        suffix = ""

                    pctl = pctl.replace(".", "_")
                    metric_key = f"{metric_type}_{pctl}{suffix}"
                    metrics[metric_key] = val
                else:
                    break

        return metrics
