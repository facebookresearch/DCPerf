#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
# pyre-strict

from benchpress.lib.parser import Parser


class TypeConversionParser(Parser):
    def parse(
        self, stdout: list[str], stderr: list[str], returncode: int
    ) -> dict[str, list[str]]:
        metrics = {"elem/s": []}
        # Since the output of the benchmark is JSON and we do need
        # every line of that file, so we simply append every line to
        # metrics.
        for line in stdout:
            metrics["elem/s"].append(line)
        return metrics
