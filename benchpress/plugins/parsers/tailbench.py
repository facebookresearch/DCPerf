#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.lib.parser import Parser


class TailBenchParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {"role": "server"}
        for line in stdout:
            if "95th percentile latency" in line:
                metrics["role"] = "client"
                p95_str, max_str = line.split("|")
                metrics["P95"] = p95_str[p95_str.rfind(" ", 0, -4) + 1 : -4]
                metrics["max"] = max_str[max_str.rfind(" ", 0, -3) + 1 : -3]
            elif "99th percentile latency" in line:
                metrics["role"] = "client"
                p99_str, mean_str = line.split("|")
                metrics["P99"] = p99_str[p99_str.rfind(" ", 0, -4) + 1 : -4]
                metrics["mean"] = mean_str[mean_str.rfind(" ", 0, -3) + 1 : -3]
            if "Optimal QPS =" in line:
                metrics["role"] = "qps_search_client"
                qps_str, lat_str = line.split(",")
                qps = qps_str[qps_str.rfind(" ") + 1 :]  # cut of "Optimal QPS ="
                lat = lat_str[lat_str.rfind(" ") + 1 :]  # cut of "achieving.."
                if "QPS" not in metrics.keys():
                    metrics["QPS"] = qps
                    metrics["P95@QPS"] = lat
                else:
                    metrics["QPS"] = ",".join([metrics["QPS"], qps])
                    metrics["P95@QPS"] = ",".join([metrics["P95@QPS"], lat])
        return metrics
