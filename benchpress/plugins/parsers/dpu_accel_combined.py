#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for run_accel_combined.sh multi-leg output.

run_accel_combined.sh runs a workload through more than one path in a
single job (host vs offload for crypto/compress; copy vs crc32c for the
SPDK data-mover) and wraps each leg in a marker line:

    ##DPU_PERF_LEG=<label>,<subparser>##
    <raw output of that leg, in the sub-tool's native format>

This parser splits on those markers, runs the named sub-parser on each
section (reusing the exact same parsing as the standalone jobs), and
namespaces every metric as ``<label>_<metric>`` — e.g.
``host_cipher_throughput_MBps_max``, ``offload_throughput_Gbps_avg``,
``copy_total_throughput_MiBps``.

When both a ``host`` and an ``offload`` leg are present and expose a
comparable throughput metric, it also emits ``offload_vs_host_speedup``
(offload / host, with the host figure unit-converted to match). A leg
that printed ``##DPU_PERF_LEG_FAILED=<label>##`` contributes a
``<label>_failed = True`` marker and is skipped for speedup.
"""

import re

from benchpress.lib.parser import Parser

from .dpdk_compress_perf import DpdkCompressPerfParser
from .dpdk_crypto_perf import DpdkCryptoPerfParser
from .lzbench_perf import LzbenchPerfParser
from .openssl_speed import OpensslSpeedParser
from .spdk_accel_perf import SpdkAccelPerfParser

_LEG_RE = re.compile(r"^##DPU_PERF_LEG=([a-z0-9_]+),([a-z0-9_]+)##\s*$")
_FAIL_RE = re.compile(r"^##DPU_PERF_LEG_FAILED=([a-z0-9_]+)##\s*$")

_SUBPARSERS = {
    "openssl_speed": OpensslSpeedParser,
    "dpdk_crypto_perf": DpdkCryptoPerfParser,
    "dpdk_compress_perf": DpdkCompressPerfParser,
    "lzbench_perf": LzbenchPerfParser,
    "spdk_accel_perf": SpdkAccelPerfParser,
}

# Ordered list of (offload_key, host_key, host_to_offload_factor) tuples
# tried for the offload-vs-host speedup. The factor converts the host
# metric into the offload metric's unit:
#   - crypto: openssl reports decimal MB/s, dpdk reports Gb/s
#     -> Gbps = MBps * 8 / 1000  => factor 0.008
#   - compress: same MB/s -> Gb/s conversion on the host (lzbench) side
_SPEEDUP_PAIRS = [
    ("offload_throughput_Gbps_avg", "host_cipher_throughput_MBps_max", 0.008),
    ("offload_compress_throughput_Gbps_avg", "host_best_compress_MBps", 0.008),
]


def _split_sections(stdout):
    """Yield (label, subparser_name, lines, failed) per leg."""
    sections = []
    cur = None
    failed = set()
    for raw in stdout:
        line = raw.rstrip("\n")
        m = _LEG_RE.match(line)
        if m:
            cur = {"label": m.group(1), "sub": m.group(2), "lines": []}
            sections.append(cur)
            continue
        mf = _FAIL_RE.match(line)
        if mf:
            failed.add(mf.group(1))
            continue
        if cur is not None:
            cur["lines"].append(line)
    for s in sections:
        yield s["label"], s["sub"], s["lines"], (s["label"] in failed)


class DpuAccelCombinedParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for label, sub, lines, failed in _split_sections(stdout):
            if failed:
                metrics[f"{label}_failed"] = True
                continue
            parser_cls = _SUBPARSERS.get(sub)
            if parser_cls is None:
                continue
            leg_metrics = parser_cls().parse(lines, [], 0)
            for k, v in leg_metrics.items():
                metrics[f"{label}_{k}"] = v

        for off_key, host_key, factor in _SPEEDUP_PAIRS:
            off = metrics.get(off_key)
            host = metrics.get(host_key)
            if isinstance(off, (int, float)) and isinstance(host, (int, float)):
                denom = host * factor
                if denom > 0:
                    metrics["offload_vs_host_speedup"] = off / denom
                    break
        return metrics
