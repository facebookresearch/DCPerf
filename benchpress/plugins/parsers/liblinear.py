#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)

# Maps the machine-parseable stdout keys emitted by run_liblinear.sh to the
# metric names exported by benchpress, along with how to coerce each value.
METRIC_SPECS = {
    "liblinear_dataset": ("dataset", str),
    "liblinear_train_instances": ("train_instances", int),
    "liblinear_train_time_sec": ("train_time_sec", float),
    "liblinear_train_throughput_instances_per_sec": (
        "throughput_instances_per_sec",
        float,
    ),
    "liblinear_test_accuracy_pct": ("accuracy_pct", float),
}

ACCURACY_REGEX = re.compile(r"Accuracy\s*=\s*([0-9.]+)")


class LibLinearParser(Parser):
    """Parse train time, throughput, and accuracy from run_liblinear.sh output.

    Example output:

        Accuracy = 96.52% (653698/677399)
        liblinear_dataset: rcv1
        liblinear_train_instances: 20242
        liblinear_train_time_sec: 1.234
        liblinear_train_throughput_instances_per_sec: 16403.6
        liblinear_test_accuracy_pct: 96.52
    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}

        for line in stdout:
            if ":" not in line:
                continue
            key, _, raw_value = line.partition(":")
            spec = METRIC_SPECS.get(key.strip())
            if spec is None:
                continue
            name, caster = spec
            value = raw_value.strip()
            if not value:
                continue
            try:
                metrics[name] = caster(value)
            except ValueError:
                logger.warning(f"Could not parse '{value}' for metric '{name}'")

        # Defensively fall back to predict's own accuracy line.
        if "accuracy_pct" not in metrics:
            for line in stdout:
                match = ACCURACY_REGEX.search(line)
                if match:
                    metrics["accuracy_pct"] = float(match.group(1))
                    break

        return metrics
