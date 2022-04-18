#!/usr/bin/env python3

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
