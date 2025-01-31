# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser

TEPS_REGEX = r"(\w+_TEPS):\s+(\d+\.?\d*e?[+-]\d*)"


class Graph500Parser(Parser):
    def parse(self, stdout, stderr, returncode):
        output = " ".join(stdout)
        metrics = {}
        times = re.findall(TEPS_REGEX, output)
        for t in times:
            metrics[t[0]] = float(t[1])
        return metrics
