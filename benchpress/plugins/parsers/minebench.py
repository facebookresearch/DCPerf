#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import re

from benchpress.lib.parser import Parser


def _minebench_regex_parser(regex, output):
    m = re.search(regex, output)
    return m.groupdict() if m else {}


def _field_map(d, keys, f):
    return {k: f(d[k]) for k in keys if k in d}


class KMeansParser(Parser):
    """Example output:
    real 2.00
    user 1.50
    sys 0.02
    """

    TIME_REGEX = (
        r"^real\s(?P<real>\d+\.\d+)"
        r"user\s(?P<user>\d+\.\d+)"
        r"sys\s(?P<sys>\d+\.\d+)"
    )

    def parse(self, stdout, stderr, returncode):
        output = "".join(stderr)
        times = _minebench_regex_parser(KMeansParser.TIME_REGEX, output)
        times = {
            "real_exec_time_secs": float(times["real"]),
            "user_exec_time_secs": float(times["user"]),
            "sys_exec_time_secs": float(times["sys"]),
        }
        return times


class PLSAParser(Parser):
    """Example output:
    Forward time: 26.47s
    BackwardFindPathsForHugeBlock Time: 7.60
    Second phase in backward period Time: 6.19

    Success!
    Total time: 40.26s
    """

    PLSA_REGEX = r"Total\stime:\s(?P<total_time>\d+\.\d+)s"

    def parse(self, stdout, stderr, returncode):
        output = "".join(stdout)
        times = _minebench_regex_parser(PLSAParser.PLSA_REGEX, output)
        times = {"total_exec_time_secs": float(times["total_time"])}
        return times


class RSearchParser(Parser):
    """Example output:
    we cost 199.2 seconds totally, 22.0 for making histogram
    Fin
    """

    RSEARCH_REGEX = r"we\scost\s(?P<total_time>\d+\.\d+)\sseconds"

    def parse(self, stdout, stderr, returncode):
        output = "".join(stdout)
        times = _minebench_regex_parser(RSearchParser.RSEARCH_REGEX, output)
        times = {"total_exec_time_secs": float(times["total_time"])}
        return times
