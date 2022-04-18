#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import re

from benchpress.lib.parser import Parser

REGEX = r"\s*\*?\d\d\.\dth:\s(\d+)\s"

logger = logging.getLogger(__name__)


class SchbenchParser(Parser):
    """
    Example output:

    Latency percentiles (usec) runtime 30 (s) (30048 total samples)
                50.0th: 16 (15765 samples)
                75.0th: 22 (7782 samples)
                90.0th: 26 (4389 samples)
                95.0th: 27 (714 samples)
                *99.0th: 31 (1166 samples)
                99.5th: 33 (134 samples)
                99.9th: 36 (71 samples)
                min=2, max=1029
    """

    def parse(self, stdout, stderr, returncode):
        stdout = stderr  # schbench writes it output on stderr
        metrics = {}

        latency_percs = ["p50", "p75", "p90", "p95", "p99", "p99_5", "p99_9"]
        # this is gross - there should be some error handling eventually
        # Find last percentile report in output
        latest_report_index = -1
        for i, l in enumerate(stdout):
            if "Latency percentiles" in l:
                latest_report_index = i

        if latest_report_index < 0:
            # No latency reports that it coudl find
            return metrics

        for key, line in zip(latency_percs, stdout[latest_report_index + 1 :]):
            match = re.match(REGEX, line)
            if match:
                key_units = key + "_microsecs"
                metrics[key_units] = float(match[1].strip())

        return metrics
