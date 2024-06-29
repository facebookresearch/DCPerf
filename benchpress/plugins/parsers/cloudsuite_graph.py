#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.lib.parser import Parser


class CloudSuiteGraphParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts cloudsuite graphics analytics results from stdout."""
        metrics = {}
        for line in stdout:
            if line.startswith("Total PageRank = "):
                metrics["pagerank"] = float(line[17:])
            if line.startswith("Running time = "):
                metrics["runtime"] = int(line[15:])
        return metrics
