#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib.parser import Parser


class ReturncodeParser(Parser):
    """Returncode parser outputs one metric 'success' that is True if job binary
    had a 0 exit code, and False all other times."""

    def parse(self, stdout, stderr, returncode):
        return {
            "success": returncode == 0,
        }
