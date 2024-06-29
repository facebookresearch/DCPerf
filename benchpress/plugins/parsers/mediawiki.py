#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.lib.baseline import BASELINES

from .generic import JSONParser

MEDIAWIKI_MLP_BASELINE = BASELINES["mediawiki"]


class MediawikiParser(JSONParser):
    def parse(self, stdout, stderr, returncode):
        metrics = super().parse(stdout, stderr, returncode)
        if "Combined" in metrics:
            if "Siege RPS" in metrics["Combined"]:
                rps = metrics["Combined"]["Siege RPS"]
                metrics["score"] = float(rps) / MEDIAWIKI_MLP_BASELINE
        return metrics
