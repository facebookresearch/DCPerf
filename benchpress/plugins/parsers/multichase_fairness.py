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

re_avg = r"avg\s+(\d+\.?\d+)"
re_sdev = r"sdev\s+(\d+\.?\d+)"


class MultichaseFairnessParser(Parser):
    """
    input format:
    cpu:     0       1       2 ...      35
    unrelaxed:
        1709.5  1730.8  1634.4 ...  1416.6 : avg 1610.2  sdev   92.6
        1708.2  1731.4  1654.3 ...  1413.3 : avg 1621.7  sdev  112.5
        1792.5  1741.9  1639.1 ...  1418.3 : avg 1617.1  sdev  106.2
        2375.7  1717.5  1645.1 ...  1404.6 : avg 1633.7  sdev  193.6
        1748.6  1724.3  1695.3 ...  1441.2 : avg 1612.6  sdev   93.0
    relaxed:
        1388.7  1344.5  1210.1 ...  1282.8 : avg 1256.2  sdev   64.4
        1269.5  1447.6  1217.4 ...  1278.4 : avg 1264.1  sdev   72.4
        1280.1  1342.7  1209.9 ...  1282.1 : avg 1246.2  sdev   52.3
        1428.4  1351.7  1212.9 ...  1282.8 : avg 1254.2  sdev   62.8
    """

    def __init__(self):
        self.fairness_total = [0]
        self.fairness_stdev_total = [0]
        self.fairness_total_relaxed = [0]
        self.fairness_stdev_total_relaxed = [0]
        # fairness run 5 iterations
        self.fairness_count = 5

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        cur_key = ""
        for line in stdout:
            if re.search("unrelaxed", line):
                cur_key = "unrelaxed"
                continue
            if re.search("relaxed", line):
                cur_key = "relaxed"
                continue
            if cur_key == "unrelaxed":
                self.collect_fairness(
                    line,
                    self.fairness_total,
                    self.fairness_stdev_total,
                    self.fairness_count,
                )
            if cur_key == "relaxed":
                self.collect_fairness(
                    line,
                    self.fairness_total_relaxed,
                    self.fairness_stdev_total_relaxed,
                    self.fairness_count,
                )

        metrics["fairness_avg_latency_atmoic_increment (ns)"] = (
            self.fairness_total[0] / self.fairness_count
        )
        metrics["fairness_stdev_latency_atmoic_increment (ns)"] = (
            self.fairness_stdev_total[0] / self.fairness_count
        )
        metrics["fairness_avg_latency_atmoic_increment_relaxed (ns)"] = (
            self.fairness_total_relaxed[0] / self.fairness_count
        )
        metrics["fairness_stdev_latency_atmoic_increment_relaxed (ns)"] = (
            self.fairness_stdev_total_relaxed[0] / self.fairness_count
        )

        return metrics

    def collect_fairness(self, line, total, stdev_total, count):
        if len(line) == 0 or re.search("latency", line) or re.search("fairness", line):
            return
        if re.search("cpu", line) or re.search("relaxed", line):
            return
        results = re.findall(re_avg, line)
        total[0] = total[0] + float(results[0].strip())
        results = re.findall(re_sdev, line)
        stdev_total[0] = stdev_total[0] + float(results[0].strip())
