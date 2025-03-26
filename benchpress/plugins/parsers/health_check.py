#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import os
import platform
import re

from benchpress.lib.parser import Parser


def recursive_set(obj, keys, value):
    ptr = obj
    for key in keys[:-1]:
        if key not in ptr:
            ptr[key] = {}
        ptr = ptr[key]
    ptr[keys[-1]] = value


class HealthCheckParser(Parser):
    def mm_mem_idle_latency(self, line, idx):
        metrics = self.metrics
        vals = line.split()
        if len(vals) < 3:
            return
        if line.startswith("Idle Latency (ns)"):
            self._curr_latency_mode = vals[4]
            if not hasattr(self, "numa_nodes"):
                self.numa_nodes = []
            # Search for "Node-X" in line like "Idle Latency (ns) - RandomInChunk       Node-0    Node-1"
            # and append them into self.numa_nodes
            for val in vals:
                if re.match(r"Node-\d+", val):
                    self.numa_nodes.append(val)
            j = idx + 1
            while self.stdout[j].strip() != "":
                line2 = self.stdout[j]
                if self.stdout[j].startswith("Node-"):
                    vals2 = line2.split()
                    node_id = vals2[0]
                    for i, val in enumerate(vals2[1:]):
                        latency_ns = float(val)
                        recursive_set(
                            metrics,
                            [
                                "memory",
                                "idle_latency",
                                self._curr_latency_mode,
                                node_id,
                                self.numa_nodes[i],
                            ],
                            latency_ns,
                        )
                j += 1

    def mm_mem_bw_lat(self, line, idx):
        metrics = self.metrics
        match = re.search(r"delay\s+bandwidth\s+latency", line)
        if match:
            random = line.split()[-1]
            metrics["memory"]["delay_bandwidth_latency"][random] = []
            j = idx + 1
            while self.stdout[j] != "":
                if self.stdout[j].split()[0].isdigit():
                    metrics["memory"]["delay_bandwidth_latency"][random].append(
                        [
                            int(self.stdout[j].split()[0]),
                            float(self.stdout[j].split()[1]),
                            float(self.stdout[j].split()[2]),
                        ]
                    )
                j += 1

    def mm_mem_peak_bw(self, line, idx):
        metrics = self.metrics
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
            mmemcpy_large = self.stdout[idx + 6]
            metrics["memory"]["memcpy_large_mb_s"] = float(mmemcpy_large.split()[2])

        if "MemCpy test - Medium" in line:
            mmemcpy_medium = self.stdout[idx + 6]
            metrics["memory"]["memcpy_medium_mb_s"] = float(mmemcpy_medium.split()[2])

    def parse_mm_mem(self, line, idx):
        self.mm_mem_idle_latency(line, idx)
        self.mm_mem_bw_lat(line, idx)
        self.mm_mem_peak_bw(line, idx)

    def parse_sleepbench(self, line, idx):
        metrics = self.metrics
        if "calls per second" in line:
            metrics["sleepbench"]["calls per second"] = float(line.split()[0])
            cpu_line = self.stdout[idx + 1]
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

    def parse_loaded_latency(self):
        if os.path.exists("/tmp/bw-lat.tsv"):
            self.metrics["memory"]["delay_bandwidth_latency"]["loaded_latency"] = []
            with open("/tmp/bw-lat.tsv", "r") as f:
                for line in f:
                    vals = line.split()
                    if len(vals) < 3:
                        continue
                    try:
                        delay = int(vals[0])
                        bw = float(vals[1])
                        lat = float(vals[2])
                        self.metrics["memory"]["delay_bandwidth_latency"][
                            "loaded_latency"
                        ].append([delay, bw, lat])
                    except ValueError:
                        continue
        if os.path.exists("/tmp/latency.txt"):
            with open("/tmp/latency.txt", "r") as f:
                for line in f:
                    # Search for "avg_latency = 179.559148 ns"
                    match = re.search(r"avg_latency = (\d+\.\d+) ns", line)
                    if match:
                        self.metrics["memory"]["idle_latency"]["avg_latency"] = float(
                            match.group(1)
                        )
                        break

    def parse(self, stdout, stderr, returncode):
        self.metrics = {}
        self.stdout = stdout
        metrics = self.metrics
        avg_ping = {}
        bitrates = {}
        metrics["ping"] = []
        metrics["iperf3_bandwidth"] = []
        metrics["sleepbench"] = {}
        metrics["memory"] = {}
        metrics["memory"]["peak_bandwidth_mb_s"] = {}
        metrics["memory"]["idle_latency"] = {}
        metrics["memory"]["delay_bandwidth_latency"] = {}
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

            self.parse_sleepbench(line, i)

            if platform.machine() != "aarch64":
                self.parse_mm_mem(line, i)

        # parse loaded_latency if the machine is aarch64
        if platform.machine() == "aarch64":
            self.parse_loaded_latency()

        for hostname, avg in avg_ping.items():
            metrics["ping"].append(
                {"hostname": hostname, "latency": float(avg), "unit": "ms"}
            )
        for hostname, bitrate in bitrates.items():
            metrics["iperf3_bandwidth"].append(
                {"hostname": hostname, "bandwidth": float(bitrate), "unit": "Gbps"}
            )
        return metrics
