#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser


_FLOAT_METRIC_PATTERNS = (
    (re.compile(r"Wall\s+time\s*/\s*call:\s+([\d.]+)\s*us"), "wall_time_per_call_us"),
    (
        re.compile(r"Host\s+overhead\s*/\s*call:\s+([\d.]+)\s*us"),
        "host_overhead_per_call_us",
    ),
    (
        re.compile(r"Simulated\s+GPU\s*/\s*call:\s+([\d.]+)\s*us"),
        "simulated_gpu_per_call_us",
    ),
    (re.compile(r"Simulated\s+TF/s:\s+([\d.]+)"), "simulated_tflops"),
    (re.compile(r"Total\s+wall\s+time:\s+([\d.]+)\s*ms"), "total_wall_time_ms"),
    (re.compile(r"Total\s+FLOPs:\s+([\d.]+)\s*T"), "total_flops_t"),
    (
        re.compile(r"Simulated\s+GPU\s+time:\s+([\d.]+)\s*ms"),
        "simulated_gpu_time_ms",
    ),
    (re.compile(r"Host\s+overhead:\s+([\d.]+)\s*ms"), "host_overhead_ms"),
    (re.compile(r"Per-iter\s+wall\s+time:\s+([\d.]+)\s*ms"), "per_iter_wall_time_ms"),
    (re.compile(r"FLOPs/iter:\s+([\d.]+)\s*T"), "flops_per_iter_t"),
)

_INT_METRIC_PATTERNS = (
    (re.compile(r"Total\s+calls:\s+([\d,]+)"), "total_calls"),
    (re.compile(r"Signatures:\s+(\d+)"), "signatures"),
    (re.compile(r"Calls/iter:\s+([\d,]+)"), "calls_per_iter"),
)


class PytorchGemmDispatchParser(Parser):
    """Parser for PyTorch GEMM dispatch benchmark output.

    Extracts metrics from:
      - Stage 1 / Stage 2 standalone GEMM output:
      Wall time / call:       13.730 us
      Host overhead / call:   13.730 us
      Simulated TF/s:         156.440000

      - Stage 2 YAML workload replay output:
      Total wall time:          1234.56 ms
      Total calls:               12,345
      Total FLOPs:                 0.456 T
      Per-iter wall time:        411.52 ms
    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}

        for line in stdout:
            line = line.strip()

            for pattern, metric_name in _FLOAT_METRIC_PATTERNS:
                m = pattern.search(line)
                if m:
                    metrics[metric_name] = float(m.group(1))
                    break
            else:
                for pattern, metric_name in _INT_METRIC_PATTERNS:
                    m = pattern.search(line)
                    if m:
                        metrics[metric_name] = int(m.group(1).replace(",", ""))
                        break

        return metrics
