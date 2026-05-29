#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for the dpu_perf `ovs_ofperf` wrapper.

run_ovs.sh's `ovs_ofperf` mode prints parser-friendly key=value lines:

    ovs_ofperf: num_flows=10000
    ovs_ofperf: bridge=dpu_perf_brX
    ovs_ofperf: elapsed_sec=2.345678
    ovs_ofperf: flows_per_sec=4263.45

Emitted metrics: num_flows (int), elapsed_sec (float),
flows_per_sec (float).
"""

import re

from benchpress.lib.parser import Parser


_KV_RE = re.compile(r"^\s*ovs_ofperf:\s*([a-z_]+)=([^\s]+)\s*$")


class OvsOfperfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for raw in stdout:
            m = _KV_RE.match(raw)
            if not m:
                continue
            key = m.group(1)
            raw_val = m.group(2)
            if key == "num_flows":
                try:
                    metrics["num_flows"] = int(raw_val)
                except ValueError:
                    pass
            elif key in ("elapsed_sec", "flows_per_sec"):
                try:
                    metrics[key] = float(raw_val)
                except ValueError:
                    pass
            elif key == "bridge":
                metrics["bridge"] = raw_val
        return metrics
