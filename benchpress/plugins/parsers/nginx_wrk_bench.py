#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

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
