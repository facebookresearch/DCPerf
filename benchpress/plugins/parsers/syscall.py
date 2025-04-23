#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib.parser import Parser


class SyscallParser(Parser):
    # Minimum hit rate for a server data point to be considered in
    # result qps calculation
    MIN_HIT_RATE = 0.88

    def __init__(self, server_csv_name="server.csv"):
        self.server_csv_name = server_csv_name

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            toks = line.strip().split(" ")
            if line.strip().endswith("calls per second"):
                metrics[toks[0] + "_calls_per_second"] = toks[1]
            elif line.strip().startswith("workers: "):
                metrics["workers"] = toks[1]

        return metrics
