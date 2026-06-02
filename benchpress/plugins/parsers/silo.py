#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for Silo's `dbtest` output.

dbtest prints results to stderr in the form:

    runtime: 1.00328 sec
    memory delta: 58.2734 MB
    memory delta rate: 58.0828 MB/sec
    logical memory delta: 3.56796 MB
    logical memory delta rate: 3.55628 MB/sec
    agg_nosync_throughput: 55954.4 ops/sec
    avg_nosync_per_core_throughput: 55954.4 ops/sec/core
    agg_throughput: 55954.4 ops/sec
    avg_per_core_throughput: 55954.4 ops/sec/core
    agg_persist_throughput: 55954.4 ops/sec
    avg_per_core_persist_throughput: 55954.4 ops/sec/core
    avg_latency: 0.0177805 ms
    avg_persist_latency: 0 ms
    agg_abort_rate: 0 aborts/sec
    avg_per_core_abort_rate: 0 aborts/sec/core
    txn breakdown: [[Delivery, 2193], [NewOrder, 25386], ...]

YCSB and the microbenchmarks omit some lines (e.g. no `txn breakdown` for
YCSB). Each metric is extracted independently and silently skipped when
absent, so the parser never raises on partial output.
"""

from __future__ import annotations

import re

from benchpress.lib.parser import Parser


# Each rule is (group, metric_name, compiled regex). The regex must have a
# single capture group that yields the float value.
_FLOAT = r"([-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)"

_RULES: list[tuple[str, str, re.Pattern]] = [
    ("throughput", "agg_throughput", re.compile(rf"^agg_throughput:\s+{_FLOAT}")),
    (
        "throughput",
        "avg_per_core_throughput",
        re.compile(rf"^avg_per_core_throughput:\s+{_FLOAT}"),
    ),
    (
        "throughput",
        "agg_nosync_throughput",
        re.compile(rf"^agg_nosync_throughput:\s+{_FLOAT}"),
    ),
    (
        "throughput",
        "agg_persist_throughput",
        re.compile(rf"^agg_persist_throughput:\s+{_FLOAT}"),
    ),
    (
        "throughput",
        "avg_per_core_persist_throughput",
        re.compile(rf"^avg_per_core_persist_throughput:\s+{_FLOAT}"),
    ),
    ("latency", "avg_latency", re.compile(rf"^avg_latency:\s+{_FLOAT}")),
    (
        "latency",
        "avg_persist_latency",
        re.compile(rf"^avg_persist_latency:\s+{_FLOAT}"),
    ),
    ("abort", "agg_abort_rate", re.compile(rf"^agg_abort_rate:\s+{_FLOAT}")),
    (
        "abort",
        "avg_per_core_abort_rate",
        re.compile(rf"^avg_per_core_abort_rate:\s+{_FLOAT}"),
    ),
    ("memory", "memory_delta", re.compile(rf"^memory delta:\s+{_FLOAT}")),
    (
        "memory",
        "memory_delta_rate",
        re.compile(rf"^memory delta rate:\s+{_FLOAT}"),
    ),
    (
        "memory",
        "logical_memory_delta",
        re.compile(rf"^logical memory delta:\s+{_FLOAT}"),
    ),
    (
        "memory",
        "logical_memory_delta_rate",
        re.compile(rf"^logical memory delta rate:\s+{_FLOAT}"),
    ),
    ("runtime", "runtime_sec", re.compile(rf"^runtime:\s+{_FLOAT}")),
]

# `txn breakdown: [[Name1, N1], [Name2, N2], ...]`
_TXN_BREAKDOWN_LINE = re.compile(r"^txn breakdown:\s*\[(.+)\]\s*$")
_TXN_BREAKDOWN_ENTRY = re.compile(r"\[\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(\d+)\s*\]")


class SiloParser(Parser):
    def parse(self, stdout, stderr, returncode):
        # dbtest writes its results to stderr. Accept either a list of lines or
        # a single string. Iterate line-by-line to match each rule at most
        # once and to keep the parser O(n) in the output size.
        if isinstance(stderr, (list, tuple)):
            lines = []
            for chunk in stderr:
                lines.extend(chunk.splitlines() if "\n" in chunk else [chunk])
        else:
            lines = (stderr or "").splitlines()

        metrics: dict = {
            "throughput": {},
            "latency": {},
            "abort": {},
            "memory": {},
            "runtime": {},
            "txn_breakdown": {},
        }

        # Track which rules we've already matched so a single line cannot fire
        # the same rule twice (dbtest prints each metric once, but be safe).
        matched: set[tuple[str, str]] = set()

        for raw_line in lines:
            line = raw_line.strip()
            if not line:
                continue

            for group, name, pattern in _RULES:
                key = (group, name)
                if key in matched:
                    continue
                m = pattern.match(line)
                if m:
                    try:
                        metrics[group][name] = float(m.group(1))
                    except ValueError:
                        continue
                    matched.add(key)
                    break

            tb = _TXN_BREAKDOWN_LINE.match(line)
            if tb:
                for entry in _TXN_BREAKDOWN_ENTRY.finditer(tb.group(1)):
                    metrics["txn_breakdown"][entry.group(1)] = int(entry.group(2))

        return metrics
