#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# main functionality is actually provided in cli/main.py
from benchpress.cli.main import main
from benchpress.lib.reporter import JSONFileReporter, StdoutReporter
from benchpress.lib.reporter_factory import ReporterFactory


def invoke_main() -> None:
    # register a default class for reporting metrics
    ReporterFactory.register("default", StdoutReporter)
    ReporterFactory.register("json_file", JSONFileReporter)
    main()


if __name__ == "__main__":
    invoke_main()  # pragma: no cover
