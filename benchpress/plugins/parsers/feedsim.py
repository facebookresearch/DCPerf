#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import re

from benchpress.lib.parser import Parser

FEEDSIM_TARGET_LATENCY = (
    r"Searching\sfor\sQPS\swhere\s(\w+)\slatency\s<=\s([0-9]+[.]?[0-9]+)"
)
FEEDSIM_FINAL_QPS_REGEX = r"final\srequested_qps\s=\s(\d+\.?\d+),\smeasured_qps\s=\s(\d+\.?\d+),\slatency\s=\s(\d+\.?\d+)"

logger = logging.getLogger(__name__)


class FeedSimParser(Parser):
    """
    Example output:

        Searching for QPS where 95p latency <= 2000 msec
        peak qps = 24, latency = 5398.7
        requested_qps = 13, measured_qps = 14, latency = 790.4
        requested_qps = 19, measured_qps = 21, latency = 1080.8
        requested_qps = 22, measured_qps = 23, latency = 1101.3
        requested_qps = 23, measured_qps = 24, latency = 1116.8
        requested_qps = 24, measured_qps = 23, latency = 1128.8
        requested_qps = 24, measured_qps = 24, latency = 1138.5
        final requested_qps = 24, measured_qps = 24, latency = 910.8
    """

    def parse(self, stdout, stderr, returncode):
        """Parse FeedSim metrics."""
        # Make it multiline output
        output = "\n".join(stdout)
        metrics = {}
        matches = re.findall(FEEDSIM_TARGET_LATENCY, output)
        for m in matches:
            if len(m) != 2:
                logger.warning("Couldn't find targets latency measurement")
                break
            metrics["target_percentile"] = m[0]
            metrics["target_latency_msec"] = float(m[1])

        matches = re.findall(FEEDSIM_FINAL_QPS_REGEX, output)
        for m in matches:
            if len(m) != 3:
                logger.warning("Couldn't find final QPS measurement")
                continue
            metrics["final_requested_qps"] = float(m[0])
            metrics["final_achieved_qps"] = float(m[1])
            metrics["final_latency_msec"] = float(m[2])

        return metrics
