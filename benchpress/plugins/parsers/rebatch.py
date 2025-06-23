#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser


class RebatchParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        # Combine stdout and stderr for parsing since the output might be in either
        all_output = stdout + stderr

        for line in all_output:
            # Extract bandwidth (GB/s)
            bw_match = re.search(r"BW:\s*(\d+\.\d+)\s*GB/s", line)
            if bw_match:
                metrics["bandwidth"] = float(bw_match.group(1))

            # Extract time per batch (microseconds)
            time_match = re.search(r"Time/batch:\s*(\d+\.\d+)\s*us", line)
            if time_match:
                metrics["time_per_batch_us"] = float(time_match.group(1))

            # # Extract operations per second (batches/s)
            # ops_match = re.search(r"ops:\s*(\d+\.\d+)\s*batches/s", line)
            # if ops_match:
            #     metrics["operations_per_second"] = float(ops_match.group(1))

        return metrics
