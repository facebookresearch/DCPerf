#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for DPDK's `dpdk-test-compress-perf` in throughput mode.

The throughput-mode table (from app/test-compress-perf/comp_perf_test_throughput.c)
is emitted after the EAL banner and per-lcore device-pairing lines. Real
DPDK 25.11 output looks like:

      lcore id Level   Comp size   Comp ratio [%]    Comp [Gbps]   Decomp [Gbps]
             1     1        1595             2.43           8.04           23.22
             4     1        1595             2.43           7.96           22.81
             ...

Columns:
  0. lcore id              integer
  1. Level                 integer (compression level used)
  2. Comp size             integer (compressed payload size, bytes)
  3. Comp ratio [%]        float   (compressed / original × 100; lower = denser)
  4. Comp [Gbps]           float   (compression throughput on that lcore)
  5. Decomp [Gbps]         float   (decompression throughput on that lcore)

We aggregate across lcores into:
  - per-lcore_rows: list of [lcore_id, level, comp_size, ratio_pct, comp_gbps, decomp_gbps]
  - compress_throughput_Gbps_avg / _min / _max
  - decompress_throughput_Gbps_avg / _min / _max
  - compression_ratio_pct (constant across rows — recorded from the first row)
  - num_lcores
"""

from benchpress.lib.parser import Parser


def _is_header_line(line: str) -> bool:
    return (
        "lcore" in line
        and "Level" in line
        and "Comp" in line
        and "ratio" in line
        and "Gbps" in line
    )


class DpdkCompressPerfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        in_table = False
        rows = []
        for raw in stdout:
            line = raw.strip()
            if not line:
                continue
            if not in_table:
                if _is_header_line(line):
                    in_table = True
                continue
            parts = line.split()
            # First non-row line after header ends the table; per-lcore data
            # rows always start with an integer (lcore id).
            try:
                lcore = int(parts[0])
            except (ValueError, IndexError):
                in_table = False
                continue
            if len(parts) < 6:
                continue
            try:
                level = int(parts[1])
                comp_size = int(parts[2])
                ratio_pct = float(parts[3])
                comp_gbps = float(parts[4])
                decomp_gbps = float(parts[5])
            except ValueError:
                continue
            rows.append([lcore, level, comp_size, ratio_pct, comp_gbps, decomp_gbps])

        if not rows:
            return metrics

        comp_vals = [r[4] for r in rows]
        decomp_vals = [r[5] for r in rows]
        metrics["num_lcores"] = len(rows)
        metrics["compression_ratio_pct"] = rows[0][3]
        metrics["compressed_size_bytes"] = rows[0][2]
        metrics["compression_level"] = rows[0][1]
        metrics["compress_throughput_Gbps_avg"] = sum(comp_vals) / len(comp_vals)
        metrics["compress_throughput_Gbps_min"] = min(comp_vals)
        metrics["compress_throughput_Gbps_max"] = max(comp_vals)
        metrics["decompress_throughput_Gbps_avg"] = sum(decomp_vals) / len(decomp_vals)
        metrics["decompress_throughput_Gbps_min"] = min(decomp_vals)
        metrics["decompress_throughput_Gbps_max"] = max(decomp_vals)
        metrics["per_lcore_rows"] = rows
        return metrics
