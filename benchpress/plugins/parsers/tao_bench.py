#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.lib.parser import Parser


class TaoBenchServerSnapshot:

    KEYS = ["fast_qps", "hit_rate", "slow_qps", "slow_qps_oom"]

    def __init__(self, line):
        if line.strip().startswith("OUT OF MEMORY"):
            self.is_oom = True
            self.valid = True
        if not line.strip().startswith("fast_qps ="):
            if not hasattr(self, "valid"):
                self.valid = False
            return
        self.is_oom = False
        self.valid = True
        items = line.split(",")
        for keyvalue in items:
            key, value = keyvalue.split("=", maxsplit=2)
            key = key.strip()
            value = value.strip()
            if key in self.KEYS:
                setattr(self, key, float(value))

    def get(self, key):
        if key in self.KEYS and not hasattr(self, key):
            return 0.0
        return getattr(self, key)


class TaoBenchParser(Parser):

    # Minimum hit rate for a server data point to be considered in
    # result qps calculation
    MIN_HIT_RATE = 0.89

    def parse(self, stdout, stderr, returncode):
        """Extracts TAO bench results from stdout."""
        metrics = {"role": "unknown"}
        server_snapshots = []
        warmup_done = False
        exec_done = False
        for line in stdout:
            items = line.split()
            # server metrics
            if line.strip().startswith("fast_qps =") or line.strip().startswith(
                "OUT OF MEMORY"
            ):
                metrics["role"] = "server"
                server_snapshots.append(TaoBenchServerSnapshot(line))
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
            self.generate_server_csv(server_snapshots)
        return metrics

    def generate_server_csv(self, server_snapshots):
        lines = []
        lines.append("seq,total_qps,fast_qps,hit_rate,slow_qps,is_oom,slow_qps_oom")

        seq = 0
        for snapshot in server_snapshots:
            fast_qps = snapshot.get("fast_qps")
            slow_qps = snapshot.get("slow_qps")
            is_oom = 1 if snapshot.is_oom else 0
            total_qps = fast_qps + slow_qps
            lines.append(
                f"{seq},{total_qps},{fast_qps},"
                + f"{snapshot.get('hit_rate')},{slow_qps},{is_oom},"
                + f"{snapshot.get('slow_qps_oom')}\n"
            )
            seq += 1

        with open("benchmarks/tao_bench/server.csv", "w") as table:
            table.writelines(lines)

    def process_server_snapshots(self, metrics, server_snapshots):
        counter = 0
        total_fast_qps = 0
        total_slow_pqs = 0
        num = 0
        for snapshot in reversed(server_snapshots):
            # Also filter out data points with low hit rate
            if (
                snapshot.get("fast_qps") > 1
                and snapshot.get("hit_rate") >= self.MIN_HIT_RATE
            ):
                counter += 1
            else:
                continue
            if counter >= 5:
                total_fast_qps += snapshot.get("fast_qps")
                total_slow_pqs += snapshot.get("slow_qps")
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
