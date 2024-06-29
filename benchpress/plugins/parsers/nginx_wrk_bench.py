#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import re

from benchpress.lib.parser import Parser

REQUESTS_REGEX = r"(Requests/sec):\s+(\d+\.?\d*)"


class NginxWrkParser(Parser):
    def parse(self, stdout, stderr, returncode):
        output = "".join(stdout)
        metrics = {}
        requests = re.findall(REQUESTS_REGEX, output)[0]
        metrics[requests[0]] = float(requests[1])

        return metrics
