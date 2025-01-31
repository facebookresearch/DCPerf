#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import re

from benchpress.lib.parser import Parser


regex_map = {
    "nreq_regex": (
        re.compile(r"Total num requests\s+(\d+)"),
        lambda m: ("nreq", m.group(1)),
    ),
    "nexception_regex": (
        re.compile(r"Num exceptions\s+(\d+)"),
        lambda m: ("nexception", m.group(1)),
    ),
    "lat_regex": (
        re.compile(r"Latency us\s+(avg|p\d+)\s+(\d+)"),
        lambda m: ("lat_us_" + m.group(1), m.group(2)),
    ),
    "slat_regex": (
        re.compile(r"Server latency us\s+(avg|p\d+)\s+(\d+)"),
        lambda m: ("slat_us_" + m.group(1), m.group(2)),
    ),
}


class SigridParser(Parser):
    """Example output:
                                   TEST trace
    Total num requests              2000
    Num exceptions                     0
    Latency us avg                  7487
    Latency us p25                  5320
    Latency us p50                  6019
    Latency us p75                  7582
    Latency us p90                 10225
    Latency us p95                 19012 HBzfYEj8uxl
    Latency us p99                 22666
    Server latency us avg           6623
    Server latency us p25           4605 GzPDwxZazOO
    Server latency us p50           5287
    Server latency us p75           6673
    Server latency us p90           9361
    Server latency us p95          16849
    Server latency us p99          20875
    """

    def parse(self, stdout, stderr, returncode):
        """Parses the replayer output to extract key metrics"""

        metrics = {}

        for line in stdout:
            for pattern in regex_map.values():
                m = pattern[0].match(line)
                if m:
                    n, v = pattern[1](m)
                    metrics[n] = v

        return metrics
