#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""SVE GFLOPS must count scalable floating point operations (FP_SCALE_OPS_SPEC, r80C0).

FP_FIXED_OPS_SPEC (r80C1) counts the non-scalable ones (scalar and Neon). On 128 bit SVE cores
one FP_SCALE_OPS_SPEC count is one operation, so SVE GFLOPS is r80C0 per second over 10^9 and
GFLOPS (any precision) is (r80C0 + r80C1) per second over 10^9.

Run from perfutils/:  python -m unittest tests.test_arm_sve_gflops
"""

import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from arm_perf import arches  # noqa: E402,F401  # registers every ARM CPU
from arm_perf.core import ARCH_REGISTRY, read_csv  # noqa: E402

SCALE_OPS = 3_000_000_000  # r80C0, FP_SCALE_OPS_SPEC, per interval
FIXED_OPS = 2_000_000_000  # r80C1, FP_FIXED_OPS_SPEC, per interval


def perf_stat_csv(interval_s, intervals=4):
    events = {
        "instructions": 10**9,
        "cycles": 10**9,
        "cpu_cycles": 10**9,
        "r80C0": SCALE_OPS,
        "r80C1": FIXED_OPS,
    }
    rows = []
    for i in range(1, intervals + 1):
        for name, value in events.items():
            rows.append(
                f"{i * interval_s:.6f},{value},,{name},{interval_s * 1e9:.0f},100.00,,,,"
            )
    return io.StringIO("\n".join(rows) + "\n")


def metric_means(arch, interval_s):
    grouped_df = read_csv(perf_stat_csv(interval_s)).groupby("event_name")
    metrics = [m for m in ARCH_REGISTRY[arch].build(grouped_df) if m is not None]
    return {m["name"]: float(m["series"].mean()) * m.get("prefix", 1.0) for m in metrics}


class TestArmSveGflops(unittest.TestCase):
    def test_sve_gflops_counts_scalable_operations(self):
        for arch in ("grace", "axion", "neoversev3"):
            for interval_s in (1.0, 0.5):
                with self.subTest(arch=arch, interval_s=interval_s):
                    means = metric_means(arch, interval_s)
                    self.assertAlmostEqual(
                        means["SVE GFLOPS (any precision)"],
                        SCALE_OPS / interval_s / 1e9,
                    )

    def test_gflops_counts_all_floating_point_operations(self):
        for arch in ("grace", "axion", "neoversev3"):
            for interval_s in (1.0, 0.5):
                with self.subTest(arch=arch, interval_s=interval_s):
                    means = metric_means(arch, interval_s)
                    self.assertAlmostEqual(
                        means["GFLOPS (any precision incl SVE)"],
                        (SCALE_OPS + FIXED_OPS) / interval_s / 1e9,
                    )


if __name__ == "__main__":
    unittest.main()
