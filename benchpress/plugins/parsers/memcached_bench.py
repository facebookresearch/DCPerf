#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.lib.parser import Parser


class MemcachedBenchParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts memcached bench results from stdout."""
        metrics = {}
        warmup_done = False
        exec_done = False
        for line in stdout:
            items = line.split()
            # server metrics
            if line.strip().startswith("items:"):
                # keep overwriting and get the last one
                metrics["role"] = "server"
                metrics["hit_latency_us"] = float(items[7].rstrip(","))
            # client metrics
            if line.strip().startswith("ALL STATS"):
                exec_done = warmup_done
                warmup_done = True
                metrics["role"] = "client"
            if exec_done and line.strip().startswith("Sets"):
                metrics["set_qps"] = float(items[1])
            elif exec_done and line.strip().startswith("Gets"):
                metrics["qps"] = float(items[1])
                metrics["qps_hit"] = float(items[2])
                metrics["qps_miss"] = float(items[3])
                if metrics["qps"] > 0:
                    metrics["hit_ratio"] = metrics["qps_hit"] / metrics["qps"]
                else:
                    metrics["hit_ratio"] = 0
        return metrics
