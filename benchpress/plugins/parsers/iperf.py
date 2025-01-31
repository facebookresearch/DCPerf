#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import re

from benchpress.lib.parser import Parser

# any int/fp before string 'Mbits/sec'
REGEX_VAL_BEFORE_MBITS = r"([-+]?\d*\.\d+|\d+) Mbits\/sec"

# any int/fp before string 'sec' with any length of space
REGEX_VAL_BEFORE_SEC = r"(\d*\.\d+|\d+) +sec"


class IperfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            if re.search("SUM", line) and re.search("Mbits/sec", line):
                if re.search("sender", line):
                    sender_bitrate = re.findall(REGEX_VAL_BEFORE_MBITS, line)
                    metrics["total_sender_bitrate_mbps"] = float(sender_bitrate[0])
                elif re.search("receiver", line):
                    receiver_bitrate = re.findall(REGEX_VAL_BEFORE_MBITS, line)
                    metrics["total_receiver_bitrate_mbps"] = float(receiver_bitrate[0])
                    runtime = re.findall(REGEX_VAL_BEFORE_SEC, line)
                    metrics["runtime_in_secs"] = float(runtime[0])
            else:
                continue
        return metrics
