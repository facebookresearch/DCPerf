#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import json
from collections.abc import Mapping

from benchpress.lib.parser import Parser


PREFIX = "HALCYON_RESULT="
MODES = {"standalone", "paired_genai"}
REQUIRED_METRICS = (
    "qps",
    "read_qps",
    "write_qps",
    "throughput_mb_s",
    "read_throughput_mb_s",
    "write_throughput_mb_s",
    "latency_ms",
    "read_latency_ms",
    "write_latency_ms",
    "cpu_util_pct",
    "disk_util_pct",
    "network_util_pct",
    "reactor_accounted_cpu_ms",
    "reactor_useful_cpu_ratio",
)


class HalcyonParser(Parser):
    def parse(
        self, stdout: list[str], stderr: list[str], returncode: int
    ) -> dict[str, float]:
        if returncode != 0:
            raise ValueError(f"Halcyon exited with status {returncode}")
        result_lines = [line for line in stdout if line.strip().startswith(PREFIX)]
        if len(result_lines) != 1:
            raise ValueError("expected exactly one Halcyon JSON result envelope")
        try:
            document = json.loads(result_lines[0].strip()[len(PREFIX) :])
            result = document["halcyon_result"]
            metrics = result["metrics"]
        except (json.JSONDecodeError, KeyError, TypeError) as error:
            raise ValueError("malformed Halcyon JSON result envelope") from error
        if result.get("schema_version") != 1 or result.get("mode") not in MODES:
            raise ValueError("unsupported Halcyon result schema or mode")
        if result.get("returncode") != 0 or result.get("partial") is not False:
            raise ValueError("Halcyon result is failed or partial")
        if not isinstance(metrics, Mapping):
            raise ValueError("Halcyon metrics must be a mapping")
        missing = set(REQUIRED_METRICS).difference(metrics)
        if missing:
            raise ValueError(f"Halcyon result is missing metrics: {sorted(missing)}")
        if not all(
            isinstance(metrics[key], (int, float))
            and not isinstance(metrics[key], bool)
            for key in REQUIRED_METRICS
        ):
            raise ValueError("Halcyon metrics must be numeric")
        if metrics["qps"] <= 0 or metrics["throughput_mb_s"] <= 0:
            raise ValueError("Halcyon QPS and throughput must be positive")
        return {name: float(metrics[name]) for name in REQUIRED_METRICS}
