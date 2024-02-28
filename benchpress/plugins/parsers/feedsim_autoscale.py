#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import json
import logging

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class FeedSimAutoscaleParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Parse FeedSim AutoScale metrics."""
        output = ""
        json_started = False
        for line in reversed(stdout):
            if line.startswith("{"):
                output = "{" + output
                break
            elif line.startswith("}"):
                json_started = True
            if json_started:
                output = line + output

        try:
            metrics = json.loads(output)
            return metrics
        except json.JSONDecodeError as e:
            logger.warning("Couldn't parse feedsim_autoscale output: " + str(e))
            logger.warning("Collected output: " + output)
