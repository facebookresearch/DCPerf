#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
import json
import logging

from benchpress.lib.baseline import BASELINES

from benchpress.lib.parser import Parser

FEEDSIM_BASELINE = BASELINES["feedsim"]

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
            if "overall" in metrics and "final_achieved_qps" in metrics["overall"]:
                final_qps = metrics["overall"]["final_achieved_qps"]
                metrics["score"] = float(final_qps) / FEEDSIM_BASELINE
            return metrics
        except json.JSONDecodeError as e:
            logger.warning("Couldn't parse feedsim_autoscale output: " + str(e))
            logger.warning("Collected output: " + output)
