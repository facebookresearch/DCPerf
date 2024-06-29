#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import re

from benchpress.lib.parser import Parser

# pick up values for different metrics
REGEX_VAL_AFTER_MEAN = r"mean (\d*)"
REGEX_VAL_AFTER_STDDEV = r"stddev (\d*)"
REGEX_VAL_AFTER_MAX = r"max (\d*)"


class SmallLocksParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        lock_name = ""
        for line in stdout:
            # haven't pick up the name, continue
            if re.search("------- ", line):
                # pick up name, adding metrics
                lock_name = line[8:]
                continue
            if re.search("mean", line):
                mean_val = re.findall(REGEX_VAL_AFTER_MEAN, line)[0]
                metrics[lock_name + " mean in us"] = float(mean_val)
                stddev_val = re.findall(REGEX_VAL_AFTER_STDDEV, line)[0]
                metrics[lock_name + " stddev in us"] = float(stddev_val)
                max_val = re.findall(REGEX_VAL_AFTER_MAX, line)[0]
                metrics[lock_name + " max in us"] = float(max_val)
        print(metrics)
        return metrics
