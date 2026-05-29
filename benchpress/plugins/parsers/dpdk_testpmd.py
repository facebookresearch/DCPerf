#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for DPDK testpmd's `--stats-period` periodic-stats output.

testpmd prints a per-port stats block every `--stats-period` seconds:

  ---------------------- Forward statistics for port 0  ----------------------
    RX-packets: 12345              RX-dropped: 0             RX-total: 12345
    TX-packets: 12345              TX-dropped: 0             TX-total: 12345
    ----------------------------------------------------------------------------

We extract the last RX/TX counts (the snapshot taken just before testpmd
exits) and compute pps from `--stats-period`-derived duration where we
can. Limitations:
  - testpmd reports cumulative packet counts, not pps directly; the
    parser captures the final cumulative counts and the operator supplies
    the duration externally (job knows it; benchpress passes the args).
  - When testpmd runs against the DPU-emulated virtio-net SDI the bench
    is fundamentally DPU-gated; see README's "Tier 1 benches deferred or
    skipped" section.
"""

import re
from typing import Dict, List

from benchpress.lib.parser import Parser


_PORT_HDR_RE = re.compile(r"Forward statistics for port\s+(\d+)")
_RX_RE = re.compile(r"RX-packets:\s+(\d+)\s+RX-dropped:\s+(\d+)")
_TX_RE = re.compile(r"TX-packets:\s+(\d+)\s+TX-dropped:\s+(\d+)")


class DpdkTestpmdParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics: Dict = {}
        # Track per-port latest counters; testpmd may emit multiple snapshots
        # under --stats-period, so we want the most recent one.
        latest_per_port: Dict[int, Dict[str, int]] = {}
        current_port = None
        for raw in stdout:
            line = raw.strip()
            if not line:
                continue
            m = _PORT_HDR_RE.search(line)
            if m:
                current_port = int(m.group(1))
                latest_per_port.setdefault(current_port, {})
                continue
            if current_port is None:
                continue
            m = _RX_RE.search(line)
            if m:
                latest_per_port[current_port]["rx_packets"] = int(m.group(1))
                latest_per_port[current_port]["rx_dropped"] = int(m.group(2))
                continue
            m = _TX_RE.search(line)
            if m:
                latest_per_port[current_port]["tx_packets"] = int(m.group(1))
                latest_per_port[current_port]["tx_dropped"] = int(m.group(2))
                continue

        if not latest_per_port:
            return metrics
        ports: List[List] = []
        rx_total = 0
        tx_total = 0
        rx_dropped_total = 0
        tx_dropped_total = 0
        for port, counts in sorted(latest_per_port.items()):
            ports.append(
                [
                    port,
                    counts.get("rx_packets", 0),
                    counts.get("rx_dropped", 0),
                    counts.get("tx_packets", 0),
                    counts.get("tx_dropped", 0),
                ]
            )
            rx_total += counts.get("rx_packets", 0)
            rx_dropped_total += counts.get("rx_dropped", 0)
            tx_total += counts.get("tx_packets", 0)
            tx_dropped_total += counts.get("tx_dropped", 0)
        metrics["num_ports"] = len(ports)
        metrics["per_port_counts"] = ports
        metrics["rx_packets_total"] = rx_total
        metrics["tx_packets_total"] = tx_total
        metrics["rx_dropped_total"] = rx_dropped_total
        metrics["tx_dropped_total"] = tx_dropped_total
        return metrics
