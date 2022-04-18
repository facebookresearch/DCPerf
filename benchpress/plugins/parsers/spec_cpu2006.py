#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import re

from benchpress.lib.parser import Parser

SPECCPU_RAWFORMAT_REGEX = r"([\w.]+):\s(.*)"
SPECCPU_RAWFORMAT_MATCHER = re.compile(SPECCPU_RAWFORMAT_REGEX)


class SPECCPU2006Parser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts SPEC CPU2006 results from rawformat report."""
        output = "\n".join(stdout)
        metrics = {}
        matches = re.findall(SPECCPU_RAWFORMAT_MATCHER, output)
        for (k, v) in matches:
            if (
                k.endswith(".base_copies")
                or k.endswith(".base_threads")
                or k.endswith(".rate")
            ):
                try:
                    metrics[k] = int(v)
                except ValueError:
                    err_msg = "Failed to parse value from {} as int. Assuming -1"
                    logging.warning(err_msg.format(k))
                    metrics[k] = -1
            elif (
                k.endswith(".basemean")
                or k.endswith(".ratio")
                or k.endswith(".reported_time")
            ):
                try:
                    metrics[k] = float(v)
                except ValueError:
                    err_msg = "Failed to parse value from {} as float. Assuming -1.0"
                    logging.warning(err_msg.format(k))
                    metrics[k] = -1.0
            elif k.endswith(".metric") or k.endswith(".name") or k.endswith(".units"):
                metrics[k] = v
            elif k.rfind("errors") != -1:
                metrics[k] = v

        return metrics
