#!/usr/bin/env python3

import re

from benchpress.lib.parser import Parser

REGEX_VAL_BEFORE_BS = r" (\d+\.?\d+?)B\/s"


class EncryptionParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        rate = []
        for line in stdout:
            speed = re.findall(REGEX_VAL_BEFORE_BS, line)
            if len(speed) != 0:
                rate.append(float(speed[0]) / 1024 / 1024)
        product = 1
        for k in rate:
            product = product * k
        geo_mean = product ** (1 / len(rate))
        metrics["geo_mean for encryption rate (MB/s)"] = geo_mean
        return metrics
