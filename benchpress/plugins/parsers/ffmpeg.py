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


class FfmpegParser(Parser):
    """
    Example output:

        level5: 0:07.36
        level6: 1:36:58
        level7: 0:20.00

    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            if re.search("res_level", line):
                level = line.split(":")[0][4:]
                level_time = level + "_time (sec)"
                level_throughput = level + "_throughput (rounds per hour)"
                time = line.split()[-1]
                if "." in time:
                    _sec = time.split(".")[0].split(":")[1]
                    _min = time.split(".")[0].split(":")[0]
                    time = int(_sec) + int(_min) * 60
                else:
                    _sec = time.split(":")[2]
                    _min = time.split(":")[1]
                    _hour = time.split(":")[0]
                    time = int(_sec) + int(_min) * 60 + int(_hour) * 3600
                metrics[level_time] = time
                metrics[level_throughput] = 3600 / float(time)
        return metrics
