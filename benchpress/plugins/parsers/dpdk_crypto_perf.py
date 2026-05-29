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

When --buffer-sz is given as a comma-separated sweep (e.g.
"64,256,1024,4096,8192,16384"), DPDK prints one row per (lcore, buf-size)
under a single header. We bucket rows by buffer size, then:
  - report the per-bucket avg/min/max as `throughput_Gbps_avg_at_<sz>` etc.
  - report the bucket with the highest avg as `throughput_Gbps_avg` (peak)
    so single-scalar consumers see the headline number, not a mix of 64 B
    and 16 KB results.

Emitted metrics:
  - per_lcore_rows: list of [lcore_id, buf_size, mops, gbps, cycles_per_buf]
  - num_lcores                    (rows per buffer size; assumed constant)
  - buffer_sizes                  (list of buf sizes observed)
  - burst_size                    (constant across rows)
  - throughput_Gbps_avg           (peak bucket avg across buffer sizes)
  - throughput_Gbps_min / _max    (across all rows)
  - throughput_Gbps_avg_at_<sz>   (per-bucket avg)
  - mops_avg                      (peak bucket avg across buffer sizes)
  - mops_min / _max               (across all rows)
  - mops_avg_at_<sz>              (per-bucket avg)
  - cycles_per_buf_avg            (across all rows)
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

        # Bucket rows by buffer size so a sweep doesn't average 64 B and
        # 16 KB results together.
        by_buf = {}
        for r in rows:
            by_buf.setdefault(r[1], []).append(r)

        gbps_vals = [r[4] for r in rows]
        mops_vals = [r[3] for r in rows]
        cyc_vals = [r[5] for r in rows]
        metrics["num_lcores"] = len(next(iter(by_buf.values())))
        metrics["buffer_sizes"] = sorted(by_buf.keys())
        metrics["burst_size"] = rows[0][2]

        peak_gbps_bucket_avg = 0.0
        peak_mops_bucket_avg = 0.0
        for buf_size in sorted(by_buf):
            bucket = by_buf[buf_size]
            bg = [r[4] for r in bucket]
            bm = [r[3] for r in bucket]
            bg_avg = sum(bg) / len(bg)
            bm_avg = sum(bm) / len(bm)
            metrics[f"throughput_Gbps_avg_at_{buf_size}"] = bg_avg
            metrics[f"mops_avg_at_{buf_size}"] = bm_avg
            if bg_avg > peak_gbps_bucket_avg:
                peak_gbps_bucket_avg = bg_avg
            if bm_avg > peak_mops_bucket_avg:
                peak_mops_bucket_avg = bm_avg

        # Headline single-scalar metrics are the peak bucket's per-lcore
        # average so a sweep summary doesn't get dragged down by the 64 B
        # tail. Single-buffer-size runs collapse to the same thing.
        metrics["throughput_Gbps_avg"] = peak_gbps_bucket_avg
        metrics["throughput_Gbps_min"] = min(gbps_vals)
        metrics["throughput_Gbps_max"] = max(gbps_vals)
        metrics["mops_avg"] = peak_mops_bucket_avg
        metrics["mops_min"] = min(mops_vals)
        metrics["mops_max"] = max(mops_vals)
        metrics["cycles_per_buf_avg"] = sum(cyc_vals) / len(cyc_vals)
        metrics["failed_enqueued_total"] = failed_enq_total
        metrics["failed_dequeued_total"] = failed_deq_total
        # per-lcore row: [lcore_id, buf_size, mops, gbps, cycles_per_buf]
        metrics["per_lcore_rows"] = [[r[0], r[1], r[3], r[4], r[5]] for r in rows]
        return metrics
