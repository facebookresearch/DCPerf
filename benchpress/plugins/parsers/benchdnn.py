#!/usr/bin/env python3

import re

from benchpress.lib.parser import Parser


class BenchdnnParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            if re.search("matmul", line):
                results = line.split(",")
                param = results[3].replace("--", "")
                key1 = param + " Gflops"
                key2 = param + " running time(ms)"
                metrics[key1] = float(results[-1].strip())
                metrics[key2] = float(results[-2].strip())
        return metrics
