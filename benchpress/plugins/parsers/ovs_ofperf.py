#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for the dpu_perf vSwitch benches (run_ovs.sh).

Each tool in run_ovs.sh emits parser-friendly `<tool>: key=value` lines:

    ovs_ofperf: num_flows=10000
    ovs_ofperf: elapsed_sec=2.345678
    ovs_ofperf: flows_per_sec=4263.45

    acl_scale: num_rules=10000
    acl_scale: sender_mbps=8231.7
    acl_scale: receiver_mbps=8228.1

Values that parse as ints stay ints; values that parse as floats become
floats; everything else stays a string. The class name is kept as
`OvsOfperfParser` for backward compatibility with the existing
benchmarks_dpu_perf.yml entry; new ovs_* benches reference the same
parser key.
"""

import re

from benchpress.lib.parser import Parser


_KV_RE = re.compile(r"^\s*[a-z_][a-z0-9_]*:\s*([a-z_][a-z0-9_]*)=([^\s]+)\s*$")


def _coerce(s: str):
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        pass
    return s


class OvsOfperfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for raw in stdout:
            m = _KV_RE.match(raw)
            if not m:
                continue
            metrics[m.group(1)] = _coerce(m.group(2))
        return metrics
