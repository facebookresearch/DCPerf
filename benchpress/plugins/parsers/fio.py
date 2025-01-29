#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json

from benchpress.lib.parser import Parser


class FioParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}

        stdout = "".join(stdout)

        results = json.loads(stdout)
        results = results["jobs"]
        for job in results:
            name = job["jobname"]
            metrics[name] = job

        return metrics
