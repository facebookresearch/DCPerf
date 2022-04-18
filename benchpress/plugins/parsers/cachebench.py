#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import re

from benchpress.lib.parser import Parser

CACHEBENCH_RAWFORMAT_REGEX = r"([\w\s\()]+):(.*)"
CACHEBENCH_RAWFORMAT_MATCHER = re.compile(CACHEBENCH_RAWFORMAT_REGEX)


class CacheBenchParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        max_read_latency = 0
        max_write_latency = 0
        for line in stdout:
            params = re.findall(CACHEBENCH_RAWFORMAT_MATCHER, line)
            if params:
                param = params[0]
                field = param[0]
                field = re.sub(" +", " ", field)
                field = field.rstrip(" ")
                field = re.sub(" ", "_", field)
                items = param[1].split()
                value = items[0].rstrip("/s,")
                metrics[field] = value
                if field.startswith("NVM_Write_Latency"):
                    temp = float(value)
                    max_write_latency = (
                        max_write_latency if max_write_latency >= temp else temp
                    )
                elif field.startswith("NVM_Read_Latency"):
                    temp = float(value)
                    max_read_latency = (
                        max_read_latency if max_read_latency >= temp else temp
                    )
        metrics["NVM_max_read_latency"] = str(max_read_latency)
        metrics["NVM_max_write_latency"] = str(max_write_latency)

        return metrics
