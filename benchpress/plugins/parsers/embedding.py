#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser


class EmbeddingParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stderr:
            match = re.search(r"BW:\s*(\d+\.\d+)\s*GB/s", line)
            if match:
                metrics["bandwidth"] = float(match.group(1))

        return metrics
