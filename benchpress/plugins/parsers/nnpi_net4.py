#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import re

from benchpress.lib.parser import Parser

# Avg inference per second: 4947.86
# regex_ips = re.compile(r'^Avg inference per second: (\d+\.?\d+?)')
regex_tinf = re.compile(r"^Avg inference duration \(ms\): (\d+(\.\d+)?)")


class NNPINet4Parser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        sum = 0.0
        n = 0
        for line in stdout:
            m = regex_tinf.search(line)
            if m:
                tinf = float(m[1])
                sum += tinf
                n += 1
        metrics["acc_inf_lat"] = sum
        metrics["count"] = n
        metrics["IPS"] = 1000 * (n / sum)
        return metrics
