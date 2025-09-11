#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class FbgemmParser_CPU_A(Parser):
    def parse(self, stdout, stderr=None, returncode=None):
        # Regex to match the relevant lines
        pattern = re.compile(
            r"^(BLAS_FP32|FBP_t)\s+m\s*=\s*(\d+)\s+n\s*=\s*(\d+)\s+k\s*=\s*(\d+)\s+Gflops\s*=\s*([\d\.]+)\s+GBytes\s*=\s*([\d\.]+)"
        )
        metrics = {}
        for line in stdout:
            match = pattern.match(line.strip())
            if match:
                type_, m, n, k, gflops, gbytes = match.groups()
                key = (type_, int(m), int(n), int(k))
                metrics[key] = {"Gflops": float(gflops), "GBytes": float(gbytes)}
        return metrics
