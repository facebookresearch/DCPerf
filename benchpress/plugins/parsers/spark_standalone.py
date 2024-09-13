#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.lib.baseline import BASELINES
from benchpress.lib.parser import Parser

SPARK_BASELINE = BASELINES["sparkbench"]


class SparkStandaloneParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts Spark Standalone results from results.txt."""
        metrics = {}
        for line in stdout:
            items = line.split(":")
            if line.strip().startswith("test-release_test"):
                test_name = items[0].strip().replace("test-release_", "")
                metrics["execution_time_" + test_name] = float(items[1].strip())

            if line.strip().startswith("queries-per-hour"):
                qph = float(items[1].strip())
                metrics["queries_per_hour"] = qph
                metrics["score"] = qph / SPARK_BASELINE
            if line.strip().startswith("worker-cores"):
                metrics["worker_cores"] = int(items[1].strip())
            if line.strip().startswith("worker-memory"):
                metrics["worker_memory"] = f"{items[1].strip()}GB"
            if line.strip().startswith("total_iops_read"):
                metrics["total_iops_read"] = int(items[1].strip())
            if line.strip().startswith("total_iops_write"):
                metrics["total_iops_write"] = int(items[1].strip())

        return metrics
