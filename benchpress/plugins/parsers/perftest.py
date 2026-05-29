#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for OFED perftest tools (ib_write_bw / ib_read_bw / ib_send_lat).

perftest writes a fixed-format table inside two `--------` separator lines.
There are two header shapes:

  BW mode (ib_write_bw, ib_read_bw, ib_send_bw):
     #bytes  #iterations  BW peak[MB/sec]  BW average[MB/sec]  MsgRate[Mpps]
     65536   5000         11800.50         11750.25            0.179

  Latency mode (ib_send_lat, ib_write_lat, ib_read_lat):
     #bytes  #iterations  t_min[usec]  t_max[usec]  t_typical[usec]  t_avg[usec]  t_stdev[usec]  99% percentile[usec]  99.9% percentile[usec]
     2       1000         1.10         1.50         1.20             1.22         0.05           1.40                  1.45

We pick the mode by inspecting the header line that names the columns, then
parse the immediately following data row.
"""

from benchpress.lib.parser import Parser


class PerftestParser(Parser):
    def parse(self, stdout, stderr, returncode):  # noqa: C901
        metrics = {}
        header = None
        # Some perftest builds report BW units in either MB/sec or Gb/sec
        # depending on the --report_gbits flag. We record whichever appears.
        bw_unit = None
        for raw in stdout:
            line = raw.strip()
            if not line:
                continue
            if "#bytes" in line and "#iterations" in line:
                header = line
                if "Gb/sec" in line:
                    bw_unit = "Gbps"
                elif "MiB/sec" in line:
                    bw_unit = "MiBps"
                elif "MB/sec" in line:
                    bw_unit = "MBps"
                continue
            if header is None:
                continue
            parts = line.split()
            # First non-header row that begins with a number is our data.
            try:
                float(parts[0])
            except (ValueError, IndexError):
                continue
            # BW row
            if "BW peak" in header:
                try:
                    metrics["bytes"] = int(parts[0])
                    metrics["iterations"] = int(parts[1])
                    bw_peak = float(parts[2])
                    bw_avg = float(parts[3])
                    msg_rate = float(parts[4])
                    suffix = bw_unit or "MBps"
                    metrics[f"bw_peak_{suffix}"] = bw_peak
                    metrics[f"bw_avg_{suffix}"] = bw_avg
                    metrics["msg_rate_Mpps"] = msg_rate
                except (ValueError, IndexError):
                    pass
                header = None
                continue
            # Latency row
            if "t_min" in header:
                try:
                    metrics["bytes"] = int(parts[0])
                    metrics["iterations"] = int(parts[1])
                    metrics["t_min_us"] = float(parts[2])
                    metrics["t_max_us"] = float(parts[3])
                    metrics["t_typical_us"] = float(parts[4])
                    metrics["t_avg_us"] = float(parts[5])
                    metrics["t_stdev_us"] = float(parts[6])
                    if len(parts) > 7:
                        metrics["t_p99_us"] = float(parts[7])
                    if len(parts) > 8:
                        metrics["t_p99_9_us"] = float(parts[8])
                except (ValueError, IndexError):
                    pass
                header = None
                continue
        return metrics
