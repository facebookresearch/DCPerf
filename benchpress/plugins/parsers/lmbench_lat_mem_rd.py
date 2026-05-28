#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for lmbench's `lat_mem_rd` output.

lat_mem_rd prints one header line (`"stride=N`) followed by two-column data
rows: `<size_MB> <latency_ns>` (whitespace-separated, decimal floats). Sizes
ramp from sub-KB to the user-supplied max in roughly powers of two with
intermediate points, so the curve has enough resolution to read off
cache-boundary inflections.

We emit:
  - the full curve as a list of [size_MB, latency_ns] pairs,
  - summary stats (min/max latency, point count),
  - representative latencies at sizes that approximate L1 / L2 / LLC / DRAM,
    picked by nearest-neighbour on the curve.
"""

from benchpress.lib.parser import Parser


# Sizes (MB) used to extract characteristic per-cache-level latencies. The
# parser picks the nearest measured point on the curve to each of these; the
# names are documentation, not assertions about a specific machine's hierarchy.
_REPRESENTATIVE_SIZES_MB = [
    (0.004, "latency_at_4KB_ns"),
    (0.064, "latency_at_64KB_ns"),
    (0.256, "latency_at_256KB_ns"),
    (1.0, "latency_at_1MB_ns"),
    (8.0, "latency_at_8MB_ns"),
    (64.0, "latency_at_64MB_ns"),
    (512.0, "latency_at_512MB_ns"),
]


def _parse_stride(line: str):
    # Header looks like: "stride=64
    try:
        return int(line.split("=", 1)[1].rstrip('"'))
    except (ValueError, IndexError):
        return None


def _parse_data_row(line: str):
    parts = line.split()
    if len(parts) != 2:
        return None
    try:
        return float(parts[0]), float(parts[1])
    except ValueError:
        return None


def _scan_curve(stdout):
    curve = []
    stride = None
    for raw in stdout:
        line = raw.strip()
        if not line:
            continue
        if line.startswith('"stride='):
            stride = _parse_stride(line)
            continue
        point = _parse_data_row(line)
        if point is not None:
            curve.append([point[0], point[1]])
    return curve, stride


def _representative_points(curve):
    out = {}
    sizes_measured = {p[0] for p in curve}
    for target_mb, name in _REPRESENTATIVE_SIZES_MB:
        # Only emit a representative point if the curve actually reached
        # this size — avoids inventing a "DRAM latency" reading from a
        # small-range run.
        if target_mb < curve[0][0] - 1e-9 or target_mb > curve[-1][0] + 1e-9:
            continue
        nearest = min(sizes_measured, key=lambda s: abs(s - target_mb))
        for size_mb, latency_ns in curve:
            if size_mb == nearest:
                out[name] = latency_ns
                break
    return out


class LmbenchLatMemRdParser(Parser):
    def parse(self, stdout, stderr, returncode):
        curve, stride = _scan_curve(stdout)
        if not curve:
            return {}

        metrics = {
            "stride_bytes": stride,
            "num_points": len(curve),
            "size_min_MB": curve[0][0],
            "size_max_MB": curve[-1][0],
            "latency_min_ns": min(p[1] for p in curve),
            "latency_max_ns": max(p[1] for p in curve),
            "curve": curve,
        }
        metrics.update(_representative_points(curve))
        return metrics
