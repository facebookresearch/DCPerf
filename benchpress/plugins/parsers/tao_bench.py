#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.lib.parser import Parser


class TaoBenchParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts TAO bench results from stdout."""
        metrics = {"role": "unknown"}
        server_snapshots = []
        warmup_done = False
        exec_done = False
        for line in stdout:
            items = line.split()
            # server metrics
            if line.strip().startswith("fast_qps ="):
                metrics["role"] = "server"
                server_snapshots.append([float(x.rstrip(",")) for x in items[2:9:6]])
            # client metrics
            if line.strip().startswith("ALL STATS"):
                exec_done = warmup_done
                warmup_done = True
                metrics["role"] = "client"
            if exec_done:
                if line.strip().startswith("Sets"):
                    metrics["set_qps"] = float(items[1])
                elif line.strip().startswith("Gets"):
                    metrics["qps"] = float(items[1])
        # calcualte server-side QPS
        if metrics["role"] == "server":
            self.process_server_snapshots(metrics, server_snapshots)
        return metrics

    def process_server_snapshots(self, metrics, server_snapshots):
        counter = 0
        total_fast_qps = 0
        total_slow_pqs = 0
        num = 0
        for (fast_qps, slow_qps) in reversed(server_snapshots):
            if fast_qps > 1:
                counter += 1
            if counter >= 5:
                total_fast_qps += fast_qps
                total_slow_pqs += slow_qps
                num += 1
            if counter >= 360 / 5 - 10:
                break
        if num > 0:
            metrics["fast_qps"] = total_fast_qps / num
            metrics["slow_qps"] = total_slow_pqs / num
        else:
            metrics["fast_qps"] = 0
            metrics["slow_qps"] = 0
        metrics["total_qps"] = metrics["fast_qps"] + metrics["slow_qps"]
        if metrics["total_qps"] > 0:
            metrics["hit_ratio"] = metrics["fast_qps"] / metrics["total_qps"]
        else:
            metrics["hit_ratio"] = 0
        metrics["num_data_points"] = num
