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

logger = logging.getLogger(__name__)


class MultichasePingpongParser(Parser):
    """
    input format:
              1       2       3       4       5       6       7       8
     0:   500.1   500.2   521.1   500.2   500.1   500.1   500.2   500.2
     1:           521.0   500.2   500.1   500.2   521.1   500.1   500.2
     2:                   500.1   500.2   543.6   521.0   500.2   521.1
     3:                           521.1   500.1   500.2   521.1   500.1 ..
     4:                                   500.2   521.1   521.1   521.0
     5:                                           500.2   500.2   521.1
     6:                                                   500.2   521.1
     7:                                                           521.0
    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        max_latency = 0
        min_latency = 40000
        total_latency = 0
        count = 0
        for line in stdout:
            if len(line) == 0 or re.search("latency", line) or re.search("times", line):
                continue
            # if it is  "1   2    3   4  .."
            if not re.search(":", line):
                continue
            latencies = line.split(" ")
            for lt in latencies:
                if len(lt) == 0 or re.search(":", lt):
                    continue
                latency = float(lt.strip())
                if latency > max_latency:
                    max_latency = latency
                if latency < min_latency:
                    min_latency = latency
                total_latency = total_latency + latency
                count = count + 1
        metrics["max_latency (ns)"] = max_latency
        metrics["min_latency (ns)"] = min_latency
        metrics["avg_latency (ns)"] = total_latency / count
        return metrics
