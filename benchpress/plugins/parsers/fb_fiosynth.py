#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
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
            logger.error(
                "Error thrown while parsing the fb_fiosynth csv file: {}".format(e)
            )

        return fiosynth_data
