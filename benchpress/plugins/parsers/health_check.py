#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import re

from benchpress.lib.parser import Parser


class HealthCheckParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        avg_ping = {}
        bitrates = {}
        metrics["ping"] = []
        metrics["iperf3_bandwidth"] = []
        metrics["sleepbench"] = {}
        metrics["memory"] = {}
        metrics["memory"]["peak_bandwidth_mb_s"] = {}
        metrics["memory"]["idle_latency"] = {}
        metrics["memory"]["delay_latency_bandwidth"] = {}
        hostname_ping = ""
        hostname_bitrate = ""

        for i, line in enumerate(stdout):
            match = re.search(r"PING (.+?)\(", line)
            if match:
                hostname_ping = match.group(1)
            match = re.search(
                r"rtt min/avg/max/mdev = (.+?)/(.+?)/(.+?)/(.+?) ms", line
            )
            if match:
                try:
                    avg_ping[hostname_ping] = float(match.group(2))
                except ValueError:
                    avg_ping[hostname_ping] = 0.0

            # Get the bandwidth
            match = re.search(r"Connecting to host (.+?), port", line)
            if match:
                hostname_bitrate = match.group(1)
            match = re.search(
                r"\[SUM\]   0.00-10.00  sec .*? (\d+(?:\.\d+)?) Gbits/sec", line
            )
            if match:
                bitrates[hostname_bitrate] = match.group(1)
            if "calls per second" in line:
                metrics["sleepbench"]["calls per second"] = float(line.split()[0])
                cpu_line = stdout[i + 1]
                metrics["sleepbench"]["usr%"] = float(cpu_line.split(",")[0])
                metrics["sleepbench"]["nice%"] = float(cpu_line.split(",")[1])
                metrics["sleepbench"]["sys%"] = float(cpu_line.split(",")[2])
                metrics["sleepbench"]["idle%"] = float(cpu_line.split(",")[3])
                metrics["sleepbench"]["iowait%"] = float(cpu_line.split(",")[4])
                metrics["sleepbench"]["irq%"] = float(cpu_line.split(",")[5])
                metrics["sleepbench"]["softirq%"] = float(cpu_line.split(",")[6])
                metrics["sleepbench"]["steal%"] = float(cpu_line.split(",")[7])
                metrics["sleepbench"]["guest%"] = float(cpu_line.split(",")[8])
                metrics["sleepbench"]["total"] = round(
                    100 - float(metrics["sleepbench"]["idle%"]), 2
                )

            if "Idle Latency (ns)" in line:
                random = line.split()[4]
                if line.split()[-1] == "Node-0":
                    metrics["memory"]["idle_latency"][random] = {
                        "Node-0": {"Node-0": ""}
                    }
                    metrics["memory"]["idle_latency"][random]["Node-0"]["Node-0"] = (
                        float(stdout[i + 1].split()[1])
                    )

                elif line.split()[-1] == "Node-1":
                    metrics["memory"]["idle_latency"][random] = {
                        "Node-0": {"Node-0": "", "Node-1": ""},
                        "Node-1": {"Node-0": "", "Node-1": ""},
                    }
                    metrics["memory"]["idle_latency"][random]["Node-0"]["Node-0"] = (
                        float(stdout[i + 1].split()[1])
                    )
                    metrics["memory"]["idle_latency"][random]["Node-0"]["Node-1"] = (
                        float(stdout[i + 1].split()[2])
                    )
                    metrics["memory"]["idle_latency"][random]["Node-1"]["Node-0"] = (
                        float(stdout[i + 2].split()[1])
                    )
                    metrics["memory"]["idle_latency"][random]["Node-1"]["Node-1"] = (
                        float(stdout[i + 2].split()[2])
                    )

            match = re.search(r"delay\s+bandwidth\s+latency", line)
            if match:
                random = line.split()[-1]
                metrics["memory"]["delay_latency_bandwidth"][random] = []
                j = i + 1
                while stdout[j] != "":
                    if stdout[j].split()[0].isdigit():
                        metrics["memory"]["delay_latency_bandwidth"][random].append(
                            [
                                int(stdout[j].split()[0]),
                                float(stdout[j].split()[1]),
                                float(stdout[j].split()[2]),
                            ]
                        )
                    j += 1

            if "all reads :" in line:
                metrics["memory"]["peak_bandwidth_mb_s"]["all reads"] = float(
                    line.split()[3]
                )
            if "3:1 read/write :" in line:
                metrics["memory"]["peak_bandwidth_mb_s"]["3:1 read/write"] = float(
                    line.split()[3]
                )
            if "2:1 read/write :" in line:
                metrics["memory"]["peak_bandwidth_mb_s"]["2:1 read/write"] = float(
                    line.split()[3]
                )
            if "1:1 read/write :" in line:
                metrics["memory"]["peak_bandwidth_mb_s"]["1:1 read/write"] = float(
                    line.split()[3]
                )

            if "MemCpy test - Large" in line:
                mmemcpy_large = stdout[i + 6]
                metrics["memory"]["memcpy_large_mb_s"] = float(mmemcpy_large.split()[2])

            if "MemCpy test - Medium" in line:
                mmemcpy_medium = stdout[i + 6]
                metrics["memory"]["memcpy_medium_mb_s"] = float(
                    mmemcpy_medium.split()[2]
                )

        for hostname, avg in avg_ping.items():
            metrics["ping"].append(
                {"hostname": hostname, "latency": float(avg), "unit": "ms"}
            )
        for hostname, bitrate in bitrates.items():
            metrics["iperf3_bandwidth"].append(
                {"hostname": hostname, "bandwidth": float(bitrate), "unit": "Gbps"}
            )
        return metrics
