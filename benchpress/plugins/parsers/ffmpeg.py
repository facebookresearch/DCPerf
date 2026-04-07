#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import re
import statistics

from benchpress.lib.baseline import BASELINES
from benchpress.lib.parser import Parser

VIDEO_TRANSCODE_SVT_BASELINE = BASELINES["video_transcode_svt"]
VIDEO_TRANSCODE_SVT_MPX_BASELINE = BASELINES["video_transcode_svt_mpx"]


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
        score_mode = "throughput"
        total_data_encoded = 0.0
        total_megapixels_encoded = 0.0
        for line in stdout:
            if re.search("^score_mode", line):
                score_mode = line.split("=")[1].strip()
            elif re.search("total_megapixels_encoded", line):
                total_megapixels_encoded = float(line.split()[-1])
            elif re.search("total_data_encoded", line):
                total_data_encoded = float(line.split()[-2])
            elif re.search("^encoder", line):
                encoder = line.split("=")[1]
            elif re.search("res_level", line):
                level = line.split(":")[0][4:]
                level_time = level + "_time_secs"
                time = line.split()[-1]
                if "." in time:
                    _frac = time.split(".")[1]
                    _sec = time.split(".")[0].split(":")[1]
                    _min = time.split(".")[0].split(":")[0]
                    time = int(_min) * 60 + int(_sec) + float("0." + _frac)
                else:
                    _sec = time.split(":")[2]
                    _min = time.split(":")[1]
                    _hour = time.split(":")[0]
                    time = int(_sec) + int(_min) * 60 + int(_hour) * 3600
                metrics[level_time] = time
                if score_mode == "megapixel":
                    level_throughput = level + "_throughput_MPxps"
                    metrics[level_throughput] = total_megapixels_encoded / float(time)
                else:
                    level_throughput = level + "_throughput_MBps"
                    metrics[level_throughput] = total_data_encoded * 1024 / float(time)
                throughputs.append(metrics[level_throughput])

        throughput_hmean = statistics.harmonic_mean(throughputs)
        throughput_gmean = statistics.geometric_mean(throughputs)
        if score_mode == "megapixel":
            metrics["throughput_all_levels_hmean_MPxps"] = throughput_hmean
            metrics["throughput_all_levels_geomean_MPxps"] = throughput_gmean
            if encoder == "svt":
                metrics["score"] = throughput_hmean / VIDEO_TRANSCODE_SVT_MPX_BASELINE
        else:
            metrics["throughput_all_levels_hmean_MBps"] = throughput_hmean
            metrics["throughput_all_levels_geomean_MBps"] = throughput_gmean
            if encoder == "svt":
                metrics["score"] = throughput_hmean / VIDEO_TRANSCODE_SVT_BASELINE
        return metrics
