#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import subprocess

from . import Monitor


class MPStat(Monitor):
    def __init__(self, interval, job_uuid):
        super(MPStat, self).__init__(interval, "mpstat", job_uuid)
        self.headers = []

    def run(self):
        args = ["mpstat", "-u", f"{self.interval}"]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, encoding="utf-8")
        super(MPStat, self).run()

    def process_output(self, line):
        """
        Process mpstat output line by line. Example output:
        ```
        Linux 5.12.0-xxxxxx (server.hostname)  10/24/2023    _x86_64_        (32 CPU)

        01:14:56 PM  CPU    %usr   %nice    %sys %iowait    %irq   %soft  %steal  %guest  %gnice   %idle
        01:14:57 PM  all    2.80    0.00    2.61    0.00    0.00    0.06    0.06    0.00    0.00   94.47
        01:14:58 PM  all    5.14    0.00    2.02    0.03    0.00    0.06    0.06    0.00    0.00   92.68
        01:14:59 PM  all    5.20    0.00    1.67    0.00    0.00    0.16    0.03    0.00    0.00   92.94
        01:15:00 PM  all    5.83    0.00    2.14    0.00    0.00    0.03    0.03    0.00    0.00   91.96
        01:15:01 PM  all    6.40    0.03    2.33    0.00    0.00    0.06    0.03    0.00    0.00   91.14
        01:15:02 PM  all    4.70    0.00    2.15    0.00    0.00    0.16    0.03    0.00    0.00   92.97
        01:15:03 PM  all    8.89    0.00    3.54    0.00    0.00    0.22    0.16    0.00    0.00   87.19
        ```
        """
        cells = line.split()
        if len(cells) >= 3 and cells[2] == "CPU":
            if len(self.headers) == 0:
                self.headers = cells[3:]
        elif len(cells) >= 3 and cells[2] == "all":
            values = cells[3:]
            if len(values) != len(self.headers):
                return
            obj = {"timestamp": f"{cells[0]} {cells[1]}"}
            for i in range(len(self.headers)):
                obj[self.headers[i]] = float(values[i])
            self.res.append(obj)
