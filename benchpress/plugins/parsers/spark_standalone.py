#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.lib.parser import Parser


class SparkStandaloneParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts Spark Standalone results from results.txt."""
        metrics = {}
        for line in stdout:
            items = line.split(":")
            if line.strip().startswith("test-release_test"):
                test_name = items[0].strip().replace("test-release_", "")
                metrics[test_name] = float(items[1].strip())
            if line.strip().startswith("worker-cores"):
                metrics["worker_cores"] = int(items[1].strip())
            if line.strip().startswith("worker-memory"):
                metrics["worker_memory"] = f"{items[1].strip()}GB"
        return metrics
