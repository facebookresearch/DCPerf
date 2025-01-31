#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class FbgemmParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        for line in stdout:
            if re.search("GOPS", line) or len(line) == 0:
                continue
            parameters = line.split(",")
            key = "M, N, K, Type (GOPS): "
            for i in range(len(parameters) - 1):
                key = key + " " + parameters[i].strip()
            metrics[key] = float(parameters[-1].strip())
        return metrics
