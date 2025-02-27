#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class MlcParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        key = None
        start_idx = 0
        for idx, line in enumerate(stdout):
            if line.startswith("Measuring"):
                # parse previous section
                if key is not None:
                    metrics[self.get_entry_name(key)] = self.parse_test(
                        key, stdout, start_idx, idx
                    )
                    key = None
                start_idx = idx + 1
                if line.startswith("Measuring idle latencies"):
                    key = "idle numa latency"
                elif line.startswith("Measuring Peak Injection Memory Bandwidths"):
                    key = "peak injection bandwidth"
                elif line.startswith("Measuring Memory Bandwidths between nodes"):
                    key = "numa bandwidth"
                elif line.startswith("Measuring Loaded Latencies"):
                    key = "loaded latency with different injection delays"
                elif line.startswith("Measuring cache-to-cache transfer latency"):
                    key = "cache2cache transfer latency"
        # parse the last section
        if key is not None:
            metrics[self.get_entry_name(key)] = self.parse_test(
                key, stdout, start_idx, len(stdout)
            )
        return metrics

    @staticmethod
    def get_entry_name(key):
        if "bandwidth" in key:
            return key + " (MB/s)"
        elif "latency" in key:
            return key + " (ns)"
        else:
            return key

    def parse_test(self, key, stdout, start_idx, end_idx):
        sub_metrics = "-"
        try:
            func_name = "parse_" + key.replace(" ", "_")
            sub_metrics = getattr(self, func_name)(stdout[start_idx:end_idx])
        except Exception as e:
            logger.error("Error thrown while parsing '{}': {}".format(key, str(e)))
        return sub_metrics

    @staticmethod
    def parse_idle_numa_latency(stdout, label="latency"):
        metrics = {}
        started = False
        src_idx = 0
        for line in stdout:
            if started and "Numa node" not in line:
                items = line.split()
                dst_idx = 0
                for value in items[1:]:
                    metrics[f"numa_{label}_{src_idx}_to_{dst_idx}"] = float(value)
                    dst_idx += 1
                src_idx += 1
            if "Numa node" in line:
                started = True
        return metrics

    @staticmethod
    def parse_peak_injection_bandwidth(stdout):
        metrics = {}
        breaker = ":"
        start = 4
        for line in stdout:
            result = line[start:].find(breaker)
            if result > 0:
                result += start
                pattern = line[:result].rstrip()
                value = float(line[result + len(breaker) :].lstrip())
                metrics[f"peak_bw_{pattern}"] = value
        return metrics

    @staticmethod
    def parse_numa_bandwidth(stdout):
        return MlcParser.parse_idle_numa_latency(stdout, "bandwidth")

    @staticmethod
    def parse_loaded_latency_with_different_injection_delays(stdout):
        metrics = {}
        started = False
        for line in stdout:
            if started:
                items = line.split()
                if len(items) > 1:
                    inject_delay = items[0]
                    latency = float(items[1])
                    bandwidth = float(items[2])
                    metrics[f"latency_with_inject_delay_{inject_delay}"] = latency
                    metrics[f"bandwidth_with_inject_delay_{inject_delay}"] = bandwidth
            if "====" in line:
                started = True
        return metrics

    @staticmethod
    def parse_cache2cache_transfer_latency(stdout):
        metrics = {}
        pattern = ""
        src_idx = 0
        for line in stdout:
            if "Local Socket L2->L2" in line:
                items = line.split("latency")
                key = f"C2C latency {items[0].strip()}"
                metrics[key] = float(items[1].strip())
            elif "Remote Socket L2->L2" in line:
                items = line.split("latency")
                pattern = " ".join([x.strip() for x in items])
                src_idx = 0
            elif pattern and "Numa Node" not in line:
                items = line.split()
                dst_idx = 0
                for value in items[1:]:
                    if value != "-":
                        key = f"C2C latency {pattern} {src_idx}_to_{dst_idx}"
                        metrics[key] = float(value)
                    dst_idx += 1
                src_idx += 1
        return metrics
