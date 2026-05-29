# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import re

from benchpress.lib.parser import Parser

# Match `<name>_TEPS:` followed by an optional non-digit token (e.g. the
# graph500 v3 validation marker `!`), then the metric value.
#
# Examples that must parse:
#   "harmonic_mean_TEPS: 5.66e+07"               (v2)
#   "bfs  harmonic_mean_TEPS:     !  7.79e+07"   (v3 with validation marker)
#   "bfs  min_TEPS:                  6.38e+07"   (v3 no marker)
#   "bfs  harmonic_stddev_TEPS:      604630"     (v3 plain integer)
TEPS_REGEX = r"(\w+_TEPS):[\s!]+(\d+\.?\d*(?:e[+-]?\d+)?)"


class Graph500Parser(Parser):
    def parse(self, stdout, stderr, returncode):
        output = " ".join(stdout)
        metrics = {}
        times = re.findall(TEPS_REGEX, output)
        for t in times:
            metrics[t[0]] = float(t[1])
        return metrics
