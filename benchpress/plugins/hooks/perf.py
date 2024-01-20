#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import os
import sys

from benchpress.lib.hook import Hook

from .perf_monitors import cpufreq, memstat, mpstat, netstat, perfstat, topdown

BP_BASEPATH = os.path.dirname(os.path.abspath(sys.argv[0]))

DEFAULT_OPTIONS = {
    "mpstat": {
        "interval": 5,
    },
    "cpufreq": {
        "interval": 5,
    },
    "perfstat": {"interval": 5, "additional_events": []},
    "netstat": {"interval": 5, "additional_counters": []},
    "memstat": {"interval": 5, "additional_counters": []},
    "topdown": {},
}

AVAIL_MONITORS = {
    "mpstat": mpstat.MPStat,
    "cpufreq": cpufreq.CPUFreq,
    "perfstat": perfstat.PerfStat,
    "netstat": netstat.NetStat,
    "memstat": memstat.MemStat,
    "topdown": topdown.TopDown,
}


class Perf(Hook):
    def before_job(self, opts, job):
        self.opts = DEFAULT_OPTIONS
        for key in DEFAULT_OPTIONS.keys():
            if not isinstance(opts, dict):
                break
            if key in opts:
                self.opts[key].update(opts[key])

        self.benchmark_metrics_dir = BP_BASEPATH + f"/benchmark_metrics_{job.uuid}"
        if not os.path.isdir(self.benchmark_metrics_dir):
            os.mkdir(self.benchmark_metrics_dir)

        self.monitors = []
        for mon_name in AVAIL_MONITORS.keys():
            MonitorClass = AVAIL_MONITORS[mon_name]
            init_args = self.opts[mon_name]
            self.monitors.append(MonitorClass(job_uuid=job.uuid, **init_args))

        for monitor in self.monitors:
            monitor.run()

    def after_job(self, opts, job):
        for monitor in self.monitors:
            monitor.terminate()
        for monitor in self.monitors:
            monitor.write_csv()
