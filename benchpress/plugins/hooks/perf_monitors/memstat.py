#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import threading
import time

from . import logger, Monitor


MULTIPLIERS = {
    "KB": 1024,
    "MB": 1024 * 1024,
    "GB": 1024 * 1024 * 1024,
    "TB": 1024 * 1024 * 1024 * 1024,
}


class MemStat(Monitor):
    def __init__(self, interval, job_uuid, additional_counters=()):
        super(MemStat, self).__init__(interval, "mem-stat", job_uuid)
        counters = {"MemTotal", "MemFree", "MemAvailable", "SwapTotal", "SwapFree"}
        self.counters = counters.union(set(additional_counters))
        self.run_collector = False

    def do_collect(self):
        meminfo = {}
        with open("/proc/meminfo", "r") as f:
            for line in f:
                cells = line.split()
                key = cells[0].strip(":")
                value = int(cells[1])
                multiplier = 1
                if len(cells) > 2 and cells[2].upper() in MULTIPLIERS:
                    unit = cells[2].upper()
                    multiplier = MULTIPLIERS[unit]
                meminfo[key] = value * multiplier
        result = {}
        result["timestamp"] = time.strftime("%I:%M:%S %p")
        for counter in self.counters:
            if counter in meminfo:
                result[counter] = meminfo[counter]
        self.res.append(result)

    def collect(self):
        if not os.path.exists("/proc/meminfo"):
            logger.warning("/proc/meminfo does not exist - will not collect memstat")
            return
        while self.run_collector:
            self.do_collect()
            time.sleep(self.interval)

    def run(self):
        self.run_collector = True
        self.proc = threading.Thread(target=self.collect, name="net-stat", args=())
        self.proc.start()

    def terminate(self):
        self.run_collector = False
        self.proc.join()
