#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for SPDK's `accel_perf` (examples/accel/perf/accel_perf).

accel_perf prints a setup block followed by a per-core CSV-shaped table
and a Total summary. Real SPDK 25.05 output:

  Accel Perf Configuration:
  Workload Type:  copy
  Transfer size:  4096 bytes
  ...
  Run time:       3 seconds

  Core,Thread             Transfers        Bandwidth           Failed      Miscompares
  ------------------------------------------------------------------------------------
  0,0                    10533642/s      41147 MiB/s                0                0
  ====================================================================================
  Total                  10533642/s      41147 MiB/s                0                0

Per-row layout: <core>,<thread>  <transfers>/s  <bandwidth> MiB/s
<failed>  <miscompares>

The `Total` row uses the literal string "Total" in place of "<core>,<thread>".

Emitted metrics:
  - workload, buffer_size_bytes, queue_depth, runtime_s
  - total_transfers_per_sec, total_throughput_MiBps
  - total_failed, total_miscompares
  - num_threads, per_thread_rows
"""

import re
from typing import List

from benchpress.lib.parser import Parser


_NUMERIC_RE = re.compile(r"^[0-9]+(?:\.[0-9]+)?$")
# Per-row: `<core>,<thread>  <transfers>/s  <bandwidth> MiB/s  <failed>  <miscompares>`
# Tolerant of decimal forms and whitespace runs.
_ROW_RE = re.compile(
    r"^\s*(?P<core>\d+),(?P<thread>\d+)\s+"
    r"(?P<xfers>[0-9]+(?:\.[0-9]+)?)/s\s+"
    r"(?P<bw>[0-9]+(?:\.[0-9]+)?)\s+MiB/s\s+"
    r"(?P<failed>\d+)\s+(?P<misc>\d+)\s*$"
)
_TOTAL_RE = re.compile(
    r"^\s*Total\s+"
    r"(?P<xfers>[0-9]+(?:\.[0-9]+)?)/s\s+"
    r"(?P<bw>[0-9]+(?:\.[0-9]+)?)\s+MiB/s\s+"
    r"(?P<failed>\d+)\s+(?P<misc>\d+)\s*$"
)


def _maybe_int(s: str):
    try:
        return int(s)
    except ValueError:
        return None


_HEADER_KEYS = {
    "workload type:": ("workload", str),
    "transfer size:": ("buffer_size_bytes", int),
    "buffer size:": ("buffer_size_bytes", int),
    "queue depth:": ("queue_depth", int),
    "run time:": ("runtime_s", int),
    "runtime:": ("runtime_s", int),
}


def _parse_header(stripped: str, metrics: dict) -> bool:
    lower = stripped.lower()
    for prefix, (name, kind) in _HEADER_KEYS.items():
        if lower.startswith(prefix):
            rhs = stripped.split(":", 1)[1].strip()
            if kind is int:
                v = _maybe_int(rhs.split()[0])
                if v is not None:
                    metrics[name] = v
            else:
                metrics[name] = rhs
            return True
    return False


def _record_total(m, metrics: dict) -> None:
    metrics["total_transfers_per_sec"] = float(m.group("xfers"))
    metrics["total_throughput_MiBps"] = float(m.group("bw"))
    metrics["total_failed"] = int(m.group("failed"))
    metrics["total_miscompares"] = int(m.group("misc"))


def _row_tuple(m) -> list:
    return [
        int(m.group("core")),
        int(m.group("thread")),
        float(m.group("xfers")),
        float(m.group("bw")),
        int(m.group("failed")),
        int(m.group("misc")),
    ]


class SpdkAccelPerfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        per_thread: List[list] = []
        for raw in stdout:
            line = raw.rstrip()
            stripped = line.strip()
            if not stripped:
                continue
            if _parse_header(stripped, metrics):
                continue
            m = _TOTAL_RE.match(line)
            if m:
                _record_total(m, metrics)
                continue
            m = _ROW_RE.match(line)
            if m:
                per_thread.append(_row_tuple(m))

        if per_thread:
            metrics["num_threads"] = len(per_thread)
            metrics["per_thread_rows"] = per_thread
            if "total_throughput_MiBps" not in metrics:
                metrics["total_throughput_MiBps"] = sum(r[3] for r in per_thread)
            if "total_transfers_per_sec" not in metrics:
                metrics["total_transfers_per_sec"] = sum(r[2] for r in per_thread)
        return metrics
