#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import re
import statistics

from benchpress.lib.baseline import BASELINES

from benchpress.lib.parser import Parser

VIDEO_TRANSCODE_SVT_BASELINE = BASELINES["video_transcode_svt"]


class FfmpegParser(Parser):
    """
    Example output:

        level5: 0:07.36
        level6: 1:36:58
        level7: 0:20.00

    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        throughputs = []
        encoder = ""
        for line in stdout:
            if re.search("total_data_encoded", line):
                total_data_encoded = float(line.split()[-2])
            elif re.search("^encoder", line):
                encoder = line.split("=")[1]
            elif re.search("res_level", line):
                level = line.split(":")[0][4:]
                level_time = level + "_time_secs"
                level_throughput = level + "_throughput_MBps"
                time = line.split()[-1]
                if "." in time:
                    _sec = time.split(".")[0].split(":")[1]
                    _min = time.split(".")[0].split(":")[0]
                    time = int(_sec) + int(_min) * 60
                else:
                    _sec = time.split(":")[2]
                    _min = time.split(":")[1]
                    _hour = time.split(":")[0]
                    time = int(_sec) + int(_min) * 60 + int(_hour) * 3600
                metrics[level_time] = time
                metrics[level_throughput] = total_data_encoded * 1024 / float(time)
                throughputs.append(metrics[level_throughput])

        throughput_hmean = statistics.harmonic_mean(throughputs)
        metrics["throughput_all_levels_hmean_MBps"] = throughput_hmean
        throughput_gmean = statistics.geometric_mean(throughputs)
        metrics["throughput_all_levels_geomean_MBps"] = throughput_gmean
        if encoder == "svt":
            metrics["score"] = throughput_hmean / VIDEO_TRANSCODE_SVT_BASELINE
        return metrics
