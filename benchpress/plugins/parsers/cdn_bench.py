#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib.parser import Parser


class CDNBenchParser(Parser):
    """The Parser returns a dictionary of metrics mapping name → value."""

    def parse(self, stdout, stderr, returncode):
        return {
            "exit_code": returncode,
        }
