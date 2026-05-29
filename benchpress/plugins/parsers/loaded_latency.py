#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for one invocation of ARM's `loaded-latency`
(github.com/ARM-software/infra-microbenchmarks).

The dpu_perf mem_subsystem wrapper runs loaded-latency once per working-set
size (and optionally once more with bandwidth threads for a latency-under-
load point), wrapping each in a ##DPU_PERF_LEG=<label>,loaded_latency##
marker. Each invocation prints (among per-interval lines) a final summary:

    Joined LATTHREAD0, avg_latency = 20.881507 ns
    Total Bandwidth = 0.000000 MB/sec
    Average Latency = 20.881507 ns

We emit the two summary numbers for the leg:
  - latency_ns       (from "Average Latency = X ns")
  - bandwidth_MBps   (from "Total Bandwidth = X MB/sec"; 0 when unloaded)

The combined parser (dpu_accel_combined) namespaces these per leg, e.g.
lat_l1_latency_ns, lat_l3_latency_ns, lat_loaded_latency_ns,
lat_loaded_bandwidth_MBps.
"""

import re

from benchpress.lib.parser import Parser


_LAT_RE = re.compile(r"^Average Latency\s*=\s*([\d.]+)\s*ns", re.IGNORECASE)
_BW_RE = re.compile(r"^Total Bandwidth\s*=\s*([\d.]+)\s*MB/sec", re.IGNORECASE)


class LoadedLatencyParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for raw in stdout:
            line = raw.strip()
            m = _LAT_RE.match(line)
            if m:
                metrics["latency_ns"] = float(m.group(1))
                continue
            mb = _BW_RE.match(line)
            if mb:
                metrics["bandwidth_MBps"] = float(mb.group(1))
        return metrics
