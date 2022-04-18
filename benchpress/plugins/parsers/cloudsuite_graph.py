#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

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
