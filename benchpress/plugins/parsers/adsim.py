#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

# pyre-unsafe
import logging

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class AdSimParser(Parser):
    """
    Example output:
    seq,requested_qps,achieved_qps,target_latency_quantile,target_latency_usec,achieved_latency_usec
    1,15.500,15.8608,P95,989000,734752.34
    2,22.750,22.6053,P95,989000,944252.54
    3,26.375,25.2552,P95,989000,1212542.66
    4,24.562,23.635,P95,989000,1073405.46
    5,23.656,23.2849,P95,989000,1001968.13
    6,23.203,22.8548,P95,989000,977495.59
    7,23.203,22.8548,P95,989000,977495
    """

    def parse(self, stdout, stderr, returncode):
        """Parse AdSim metrics."""
        # Parse the CSV
        final_line = stdout[-1]
        while len(stdout) > 0 and len(final_line.lstrip().rstrip()) == 0:
            stdout.pop()
            final_line = stdout[-1]

        metrics = {}
        try:
            cells = final_line.split(",")
            metrics["final_requested_qps"] = float(cells[1])
            metrics["final_achieved_qps"] = float(cells[2])
            metrics["target_latency_percentile"] = cells[3]
            metrics["target_latency_msec"] = float(cells[4]) / 1000
            metrics["final_latency_msec"] = float(cells[5]) / 1000
            metrics["success"] = True
        except Exception as e:
            logger.warning("Cannot parse AdSim results: %s, %s" % (type(e), e))
            metrics["success"] = False

        return metrics
