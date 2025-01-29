#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json

from benchpress.lib.baseline import BASELINES

from benchpress.lib.parser import Parser

TAO_BENCH_BASELINE = BASELINES["taobench"]


class TaoBenchAutoscaleParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts TAO bench results from stdout."""
        metrics = {}
        jsontext = ""
        met_json = False
        for line in stdout:
            if line.strip() == "{":
                met_json = True
            if met_json:
                jsontext += line
            if line.strip() == "}":
                break
        try:
            metrics = json.loads(jsontext)
            if "total_qps" in metrics:
                metrics["score"] = float(metrics["total_qps"]) / TAO_BENCH_BASELINE
        except Exception:
            pass

        return metrics
