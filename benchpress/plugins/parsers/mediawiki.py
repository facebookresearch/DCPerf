#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib.baseline import BASELINES

from .generic import JSONParser

MEDIAWIKI_MLP_BASELINE = BASELINES["mediawiki"]


class MediawikiParser(JSONParser):
    MEDIAWIKI_MIN_AVAILABILITY = 0.95

    def parse(self, stdout, stderr, returncode):
        metrics = super().parse(stdout, stderr, returncode)
        if "Combined" in metrics:
            nginx_hits = metrics["Combined"]["Nginx hits"]
            nginx_200 = metrics["Combined"]["Nginx 200"]
            availability = nginx_200 / nginx_hits
            is_good_run = True
            if availability < self.MEDIAWIKI_MIN_AVAILABILITY:
                metrics["error"] = (
                    f"Too many unsuccessful requests, availability was {100 * availability:.2f}%"
                )
                is_good_run = False
            if "Siege RPS" in metrics["Combined"]:
                if not is_good_run:
                    metrics["Combined"]["Siege RPS"] = 0
                rps = metrics["Combined"]["Siege RPS"]
                metrics["score"] = float(rps) / MEDIAWIKI_MLP_BASELINE
            elif "Wrk RPS" in metrics["Combined"]:
                if not is_good_run:
                    metrics["Combined"]["Wrk RPS"] = 0
                rps = metrics["Combined"]["Wrk RPS"]
                metrics["score"] = float(rps) / MEDIAWIKI_MLP_BASELINE
        return metrics
