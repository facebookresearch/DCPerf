#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser


class DeserParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}

        # Parse stdout for operations per second
        for line in stdout:
            # Match "Millions of Operations per Second: X Mops/sec"
            match = re.search(
                r"Millions of Operations per Second:\s*(\d+\.\d+)\s*Mops/sec", line
            )
            if match:
                metrics["Mops/sec"] = float(match.group(1))
                break

        return metrics
