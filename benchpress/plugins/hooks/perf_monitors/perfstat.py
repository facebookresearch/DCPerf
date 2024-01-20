#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import subprocess
import time

from . import Monitor


class SoftReadOnlyList:
    def __init__(self, items, default=""):
        self.items = list(items)
        self.default = default

    def __getitem__(self, i):
        try:
            return self.items[i]
        except IndexError:
            return self.default


def unpack_perf_stat_line(line, delim=","):
    elems = SoftReadOnlyList(line.split(delim))
    return {
        "interval": elems[0],
        "counter-value": elems[1],
        "unit": elems[2],
        "event": elems[3],
        "event-runtime": elems[4],
        "pcnt-running": elems[5],
        "metric-value": elems[6],
        "metric-unit": elems[7],
    }


class PerfStat(Monitor):
    def __init__(self, interval, job_uuid, additional_events=(), delim=","):
        super(PerfStat, self).__init__(interval, "perf-stat", job_uuid)
        self.events = ["instructions", "cycles"] + list(additional_events)
        self.delim = delim

    def process_output(self, line):
        obj = unpack_perf_stat_line(line, self.delim)
        event_name = obj["event"]
        event_value = obj["counter-value"]
        if (
            len(self.res) == 0
            or abs(float(obj["interval"]) - self.res[-1]["interval"]) >= 1e-5
        ):
            self.res.append(
                {
                    "interval": float(obj["interval"]),
                    event_name: event_value,
                    "timestamp": time.strftime("%I:%M:%S %p"),
                }
            )
        else:
            self.res[-1][event_name] = event_value
        # Calculate IPC if both "instructions" and "cycles" exist
        if "instructions" in self.res[-1] and "cycles" in self.res[-1]:
            instructions = float(self.res[-1]["instructions"])
            cycles = float(self.res[-1]["cycles"])
            ipc = instructions / cycles
            self.res[-1]["instructions_per_cycle"] = ipc

    def run(self):
        args = [
            "perf",
            "stat",
            "-e",
            ",".join(self.events),
            "-I",
            f"{self.interval * 1000}",
            "-a",
            "-x",
            self.delim,
            "--log-fd",
            "1",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, encoding="utf-8")
        super(PerfStat, self).run()
