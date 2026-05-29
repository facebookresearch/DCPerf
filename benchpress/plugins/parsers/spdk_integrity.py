#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for the dpu_perf storage-integrity runner (spdk_nvmf_fio_runner.sh).

The runner emits one fio JSON document with two named jobs:
  - "baseline"  — no integrity (plain malloc bdev / plain NVMe-oF)
  - "dif" or "ddgst" — the integrity-enabled leg

This parser pulls IOPS + bandwidth for each leg (summing read+write so it's
workload-agnostic) and computes the isolated integrity overhead:

    <treatment>_overhead_pct = (baseline_iops - treatment_iops)
                               / baseline_iops * 100

Because the backing bdev is an in-DRAM malloc bdev, neither leg depends on
any real storage medium — the overhead reflects the DIF/digest cost on this
DPU's data path, not disk speed.

Emitted metrics:
  - baseline_iops, baseline_bw_MiBps
  - <treatment>_iops, <treatment>_bw_MiBps   (treatment in {dif, ddgst})
  - <treatment>_overhead_pct
"""

import json

from benchpress.lib.parser import Parser


def _job_iops_bw(job):
    """Sum read+write IOPS and bandwidth (KiB/s -> MiB/s) for a fio job."""
    iops = 0.0
    bw_kibps = 0.0
    for direction in ("read", "write"):
        d = job.get(direction, {})
        iops += float(d.get("iops", 0.0))
        bw_kibps += float(d.get("bw", 0.0))  # fio reports bw in KiB/s
    return iops, bw_kibps / 1024.0


class SpdkIntegrityParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        text = "".join(stdout)
        try:
            results = json.loads(text)
        except (ValueError, TypeError):
            return metrics

        jobs = {j.get("jobname"): j for j in results.get("jobs", [])}
        if "baseline" not in jobs:
            return metrics

        base_iops, base_bw = _job_iops_bw(jobs["baseline"])
        metrics["baseline_iops"] = base_iops
        metrics["baseline_bw_MiBps"] = base_bw

        for treatment in ("dif", "ddgst"):
            if treatment not in jobs:
                continue
            t_iops, t_bw = _job_iops_bw(jobs[treatment])
            metrics[f"{treatment}_iops"] = t_iops
            metrics[f"{treatment}_bw_MiBps"] = t_bw
            if base_iops > 0:
                metrics[f"{treatment}_overhead_pct"] = (
                    (base_iops - t_iops) / base_iops * 100.0
                )
        return metrics
