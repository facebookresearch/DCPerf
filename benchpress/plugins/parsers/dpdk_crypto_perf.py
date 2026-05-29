#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for DPDK's `dpdk-test-crypto-perf` in throughput mode.

Throughput-mode output (from app/test-crypto-perf/cperf_test_throughput.c)
emits a fixed per-lcore table after the EAL banner and per-option dump. Real
DPDK 25.11 output looks like:

      lcore id    Buf Size  Burst Size    Enqueued    Dequeued  Failed Enq  Failed Deq        MOps        Gbps  Cycles/Buf
             1        1024          32      100000      100000           0           0    4.583819   37.550642      218.16
             2        1024          32      100000      100000           0           0    4.568119   37.422031      218.91

The columns are positional (10 fields):
  0. lcore id        (int)
  1. Buf Size        (int)
  2. Burst Size      (int)
  3. Enqueued        (int)
  4. Dequeued        (int)
  5. Failed Enq      (int)
  6. Failed Deq      (int)
  7. MOps            (float, millions of ops/sec)
  8. Gbps            (float, throughput)
  9. Cycles/Buf      (float)

We parse by column position once we've identified the header.

Emitted metrics:
  - per_lcore_rows: list of [lcore_id, mops, gbps, cycles_per_buf]
  - num_lcores
  - buffer_size, burst_size (constant across rows)
  - throughput_Gbps_avg / _min / _max
  - mops_avg / _min / _max
  - cycles_per_buf_avg
  - failed_enqueued_total, failed_dequeued_total (zero on success)
"""

from benchpress.lib.parser import Parser


def _is_header_line(line: str) -> bool:
    return "lcore" in line and "Burst" in line and "MOps" in line and "Gbps" in line


class DpdkCryptoPerfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        in_table = False
        rows = []
        failed_enq_total = 0
        failed_deq_total = 0
        for raw in stdout:
            line = raw.strip()
            if not line:
                continue
            if not in_table:
                if _is_header_line(line):
                    in_table = True
                continue
            parts = line.split()
            try:
                lcore = int(parts[0])
            except (ValueError, IndexError):
                in_table = False
                continue
            # 10-column DPDK >= ~21.05 throughput schema. Skip rows that
            # don't match (e.g. an unexpected footer line that begins with
            # a digit but has fewer columns).
            if len(parts) < 10:
                continue
            try:
                buf_size = int(parts[1])
                burst_size = int(parts[2])
                failed_enq = int(parts[5])
                failed_deq = int(parts[6])
                mops = float(parts[7])
                gbps = float(parts[8])
                cycles_per_buf = float(parts[9])
            except ValueError:
                continue
            rows.append([lcore, buf_size, burst_size, mops, gbps, cycles_per_buf])
            failed_enq_total += failed_enq
            failed_deq_total += failed_deq

        if not rows:
            return metrics

        gbps_vals = [r[4] for r in rows]
        mops_vals = [r[3] for r in rows]
        cyc_vals = [r[5] for r in rows]
        metrics["num_lcores"] = len(rows)
        metrics["buffer_size"] = rows[0][1]
        metrics["burst_size"] = rows[0][2]
        metrics["throughput_Gbps_avg"] = sum(gbps_vals) / len(gbps_vals)
        metrics["throughput_Gbps_min"] = min(gbps_vals)
        metrics["throughput_Gbps_max"] = max(gbps_vals)
        metrics["mops_avg"] = sum(mops_vals) / len(mops_vals)
        metrics["mops_min"] = min(mops_vals)
        metrics["mops_max"] = max(mops_vals)
        metrics["cycles_per_buf_avg"] = sum(cyc_vals) / len(cyc_vals)
        metrics["failed_enqueued_total"] = failed_enq_total
        metrics["failed_dequeued_total"] = failed_deq_total
        # per-lcore row: [lcore_id, mops, gbps, cycles_per_buf]
        metrics["per_lcore_rows"] = [[r[0], r[3], r[4], r[5]] for r in rows]
        return metrics
