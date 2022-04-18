# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import re

from benchpress.lib.parser import Parser

TIME_REGEX = r"(\w+\sTime):\s+(\d+\.?\d*)"


class GAPBSParser(Parser):
    def parse(self, stdout, stderr, returncode):
        output = " ".join(stdout)
        metrics = {}
        times = re.findall(TIME_REGEX, output)
        for t in times:
            key = self.snakeify_name(t[0])
            metrics[key] = float(t[1])
        return metrics

    def snakeify_name(self, s):
        return "_".join(s.strip().lower().split())
