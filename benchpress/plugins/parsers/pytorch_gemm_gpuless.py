#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser


class PytorchGemmGpulessParser(Parser):
    """Parser for pytorch_gemm_gpuless benchmark output.

    Extracts metrics from both Stage 1 (TorchDispatchMode) and
    Stage 2 (mock_cuda) output formats:
      Wall time / call:       13.730 us
      Host overhead / call:   13.730 us
      Simulated TF/s:         156.440000
    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}

        for line in stdout:
            line = line.strip()

            m = re.search(r"Wall\s+time\s*/\s*call:\s+([\d.]+)\s*us", line)
            if m:
                metrics["wall_time_per_call_us"] = float(m.group(1))
                continue

            m = re.search(r"Host\s+overhead\s*/\s*call:\s+([\d.]+)\s*us", line)
            if m:
                metrics["host_overhead_per_call_us"] = float(m.group(1))
                continue

            m = re.search(r"Simulated\s+TF/s:\s+([\d.]+)", line)
            if m:
                metrics["simulated_tflops"] = float(m.group(1))
                continue

            m = re.search(r"Simulated\s+GPU\s*/\s*call:\s+([\d.]+)\s*us", line)
            if m:
                metrics["simulated_gpu_per_call_us"] = float(m.group(1))
                continue

        return metrics
