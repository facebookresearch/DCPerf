#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import csv
import io
import logging

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class Fiosynth_Parser(Parser):
    """Example output:
    CSV file with one header and multiple rows:
    Jobname,Read_IOPS,Read_BW,Write_IOPS,...,P99.9999_Trim_Latency
    RandomRead_QD001_run1,132.419483,529,0.0,0,7524.119594236,...,0.0
    """

    def parse(self, stdout, stderr, returncode):
        """Parses the fb_fiosynth benchmark output to extract key metrics"""

        stdout = "\n".join(stdout)
        try:
            fiosynth_reader = csv.DictReader(io.StringIO(stdout))
            fiosynth_data = [metrics for metrics in fiosynth_reader]
        except Exception as e:
            logger.error(f"Error thrown while parsing the fb_fiosynth csv file: {e}")

        return fiosynth_data
