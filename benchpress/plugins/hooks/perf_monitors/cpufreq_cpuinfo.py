#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import os
import threading
import time

from . import logger, Monitor


class CPUFreq(Monitor):
    def __init__(self, interval, job_uuid):
        super().__init__(interval, "cpufreq_cpuinfo", job_uuid)
        self.run_freq_collector = False
        self.supported = os.path.exists(
            "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq"
        )
        self.cpus = os.sched_getaffinity(0)

    def do_collect(self):
        if not self.supported:
            return
        freqs = []
        for cpu in self.cpus:
            with open(
                f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/cpuinfo_cur_freq"
            ) as f:
                freqs.append(float(f.read()))

        avg_freq_mhz = sum(freqs) / len(freqs) / 1000
        self.res.append(
            {"timestamp": time.strftime("%I:%M:%S %p"), "cpufreq_mhz": avg_freq_mhz}
        )

    def collector(self):
        while self.supported and self.run_freq_collector:
            try:
                self.do_collect()
            except ValueError:
                pass
            time.sleep(self.interval)

    def run(self):
        if not self.supported:
            logger.warning(
                "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq is "
                + "not found, therefore we cannot monitor CPU frequency"
            )
        self.run_freq_collector = True
        self.proc = threading.Thread(
            target=self.collector, name="cpufreq_cpuinfo", args=()
        )
        self.proc.start()

    def terminate(self):
        self.run_freq_collector = False
        self.proc.join()
