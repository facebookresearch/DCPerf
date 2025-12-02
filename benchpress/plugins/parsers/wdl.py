#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import re

from benchpress.lib.parser import Parser


class WDLParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            if re.search("score:", line):
                benchmark = line.split()[0]
                metrics[benchmark] = float(line.split(":")[-1])

        metrics["detailed results in each out_benchmark_name.json file"] = " "

        return metrics
