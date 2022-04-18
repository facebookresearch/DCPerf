#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.lib.parser import Parser


class StreamParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        patterns = {"copy": 2, "scale": 2, "add": 3, "triad": 3}

        element_size = 8
        array_size = 75000000
        for line in stdout:
            if line.startswith("This system uses "):
                element_size = int(line.split()[3])
                continue
            if line.startswith("Array size = "):
                array_size = int(line.split()[3])
                continue
            for pattern in patterns.keys():
                if line.startswith(pattern.title()):
                    metrics[f"{pattern}_best_MBps"] = float(line.split()[1])
                    # stdout gives best rate using 1e6 as 2^20;
                    # be consistent when calculating avg & worst rates
                    num_bytes = element_size * array_size * patterns[pattern] / 1000000
                    metrics[f"{pattern}_avg_MBps"] = num_bytes / float(line.split()[2])
                    metrics[f"{pattern}_worst_MBps"] = num_bytes / float(
                        line.split()[4]
                    )
                    break
        return metrics
