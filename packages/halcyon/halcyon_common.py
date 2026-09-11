#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import re
import socket
import time
from collections import defaultdict

from cea.halcyon.py3.halcyon.thrift_types import (
    BenchmarkConfiguration,
    FileLayout,
    HalcyonException,
    HalcyonReturnCode,
    IoSizeRange,
    NodeConfig,
    PairRole,
)


kRequiredKeys = [
    "read_ratio",
    "write_ratio",
    "read_sizes",
    "write_sizes",
    "mount_points",
    "file_layout",
    "target_host",
    "pair_role",
]

DECIMAL_CAPACITY = [
    (1000**5, "P", "PB"),
    (1000**4, "T", "TB"),
    (1000**3, "G", "GB"),
    (1000**2, "M", "MB"),
    (1000**1, "K", "KB"),
    (1000**0, "bytes", "bytes"),
]


BINARY_CAPACITY = [
    (1024**5, "P", "PiB"),
    (1024**4, "T", "TiB"),
    (1024**3, "G", "GiB"),
    (1024**2, "M", "MiB"),
    (1024**1, "K", "KiB"),
    (1024**0, "bytes", "bytes"),
]


def epoch_to_iso(seconds):
    """
    Converts epoch time in seconds to an ISO timestamp string
    """
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seconds))


def str_to_bytes(text):
    """
    Converts a simple string represtation to integer bytes (e.g. 12k -> 12288)
    """
    result = None
    suffix = ["b", "k", "m", "g", "t", "p"]
    last_char = text[-1]
    if last_char in suffix:
        result = int(text[:-1]) * (1024 ** suffix.index(last_char))
    elif last_char.isdigit():
        result = int(text)
    return result


def human_readable_bytes(total_bytes, precision=1, decimal_units=True, short=False):
    """
    Converts capacity value in bytes to human readable units

    @param: total_bytes: value to convert
    @param: precision: number of fractional digits to include
    @param: decimal_units: use base-10 units, i.e. KB, MB, etc.  If set to False
        base-2 units are used, i.e. KiB, MiB, etc.
    @param: short: produce terse results, e.g. '12K' instead of '12 KB'
    @type: total_bytes: int
    @type: decimal_units: bool
    @type: short: bool
    @returns: a string representing total_bytes in human reabable form
    """
    if total_bytes < 0:
        total_bytes = 0

    if decimal_units:
        conversion = DECIMAL_CAPACITY
    else:
        conversion = BINARY_CAPACITY

    factor = 1
    suffix = ""
    for f, s, l in conversion:
        if total_bytes >= f:
            factor = f
            suffix = s if short else l
            break
        suffix = s if short else l

    value = total_bytes / factor
    if value == 0:
        precision = 0
    if precision < 1 and value > 0:
        return f"{int(value):>d}{suffix}"

    return f"{value:>.{precision}f} {suffix}"


def human_readable_latency(latency_usec, precision=1):
    """
    Converts latency in usec to convenient human readable units

    @param: latency_usec: value to convert
    @param: precision: number of fractional digits to include
    @type: total_bytes: int
    @returns: a string representing latency_usec in human reabable form
    """
    if latency_usec < 0:
        latency_usec = 0

    factor = 0
    suffix = "us"
    if latency_usec > 1000**2:
        factor = 2
        suffix = "s"
    elif latency_usec > 1000:
        factor = 1
        suffix = "ms"

    value = latency_usec / (1000**factor)
    if value == 0:
        precision = 0
    if precision < 1 and value > 0:
        return f"{int(value):>d}{suffix}"

    return f"{value:>.{precision}f} {suffix}"


def range_to_bounds(text):
    """
    Converts a capacity range string to two capacity values.
    Example: 12k-1m -> (12288, 1048576)
    """
    m = re.match(r"(?P<lower>\d+[bkmgtp]?)(-(?P<upper>\d+[bkmgtp]?))?", text)
    matches = m.groupdict()
    lower_text = matches["lower"]
    upper_text = matches["upper"]
    lower = str_to_bytes(lower_text)
    upper = lower if upper_text is None else str_to_bytes(upper_text)
    return (lower, upper)


def dict_to_IoSizeRanges(d):
    """
    Formats JSON config data to Thrift IoSizeRange struct
    """
    sizes = []
    for text, perc in d.items():
        lower, upper = range_to_bounds(text.lower())
        io_size_range = IoSizeRange(
            text=text, lowerBound=lower, upperBound=upper, percentage=perc
        )
        sizes.append(io_size_range)
    return sizes


def merge_IoSizeRanges(d1, d2):
    """
    Merges two sets of IoSizeRange data and normalizes
    """
    data = defaultdict(list)
    total_perc = 0.0
    for i in [d1, d2]:
        for text, perc in i.items():
            lower, upper = range_to_bounds(text.lower())
            total_perc += perc
        s = f"{lower}_{upper}"
        data[s].append(
            IoSizeRange(text=text, lowerBound=lower, upperBound=upper, percentage=perc)
        )
    sizes = []
    for k in sorted(data.keys()):
        v = data[k]
        perc = 100 * sum(i.percentage for i in v) / total_perc
        io_size_range = IoSizeRange(
            text=v[0].text,
            lowerBound=v[0].lowerBound,
            upperBound=v[0].upperBound,
            percentage=perc,
        )
        sizes.append(io_size_range)
    return sizes


def dict_to_FileLayout(d):
    """
    Formats JSON config data to Thrift FileLayout struct
    """
    result = FileLayout(
        maxFileSize=d["max_file_size"],
        dirs=d["dirs"],
        subdirs=d["subdirs"],
        filesPerDir=d["files_per_dir"],
    )
    return result


class BenchmarkRunStats:
    """
    A class for handling partial results while the benchmark
    is running
    """

    def __init__(self, stats=None, ts=None):
        """
        Starts with a Thrift PerfStats object, and aggregates
        the data
        """
        self.stats = stats
        self.ts = ts
        self.readIos = 0
        self.writeIos = 0
        self.readBytes = 0
        self.writeBytes = 0
        self.readUsec = 0
        self.writeUsec = 0
        # Hypernode-style reactor-loop CPU accounting summed across all
        # reactors. usefulBusyNs excludes polling-loop overhead so the ratio
        # busy/(busy+idle) reports actual work, not busy-poll floor.
        self.usefulBusyNs = 0
        self.usefulIdleNs = 0
        self.readCrcCount = 0
        self.writeCrcCount = 0
        self.readCrcNanos = 0
        self.writeCrcNanos = 0
        self.sendIos = 0
        self.recvIos = 0
        self.sendBytes = 0
        self.recvBytes = 0
        self.sendUsec = 0
        self.recvUsec = 0
        self.cpuStats = []
        self.cpuUtil = []
        self.diskUtil = []
        self.netUtil = []
        self.netXput = []
        if stats is not None:
            self.add_stats(stats)

    def add_stats(
        self,
        stats,
        netstats,
        cpu_stats=None,
        disk_util=None,
        net_util=None,
        net_xput=None,
    ):
        for i in stats.istats:
            for j in i:
                self.readIos += j.readIos
                self.writeIos += j.writeIos
                self.readBytes += j.readBytes
                self.writeBytes += j.writeBytes
                self.readUsec += j.readUsec
                self.writeUsec += j.writeUsec
                # Reactor-loop accounting is populated per-reactor on slot 0;
                # other slots keep the default 0 so a naive sum works.
                self.usefulBusyNs += getattr(j, "usefulBusyNs", 0) or 0
                self.usefulIdleNs += getattr(j, "usefulIdleNs", 0) or 0
        for i in stats.cstats:
            for j in i:
                self.readCrcCount += j.readCount
                self.writeCrcCount += j.writeCount
                self.readCrcNanos += j.readNanos
                self.writeCrcNanos += j.writeNanos
        self.sendIos += netstats.sendIos
        self.recvIos += netstats.recvIos
        self.sendBytes += netstats.sendBytes
        self.recvBytes += netstats.recvBytes
        self.sendUsec += netstats.sendUsec
        self.recvUsec += netstats.recvUsec
        if cpu_stats is not None:
            self.cpuStats.append(cpu_stats)
        # if cpu_util is not None:
        #     self.cpuUtil.append(cpu_util)
        if disk_util is not None:
            self.diskUtil.append(disk_util)
        if net_util is not None:
            self.netUtil.append(net_util)
        if net_xput is not None:
            self.netXput.append(net_xput)

    def get_total_ios(self):
        return self.readIos + self.writeIos

    def get_total_bytes(self):
        return self.readBytes + self.writeBytes

    def get_total_usec(self):
        return self.readUsec + self.writeUsec

    def get_qps(self, brstat):
        total_ios = brstat.get_total_ios() - self.get_total_ios()
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else total_ios / elapsed

    def get_read_qps(self, brstat):
        read_ios = brstat.readIos - self.readIos
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else read_ios / elapsed

    def get_write_qps(self, brstat):
        write_ios = brstat.writeIos - self.writeIos
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else write_ios / elapsed

    def get_xput(self, brstat):
        total_bytes = brstat.get_total_bytes() - self.get_total_bytes()
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else total_bytes / elapsed

    def get_read_xput(self, brstat):
        read_bytes = brstat.readBytes - self.readBytes
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else read_bytes / elapsed

    def get_write_xput(self, brstat):
        write_bytes = brstat.writeBytes - self.writeBytes
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else write_bytes / elapsed

    def get_latency(self, brstat):
        total_ios = brstat.get_total_ios() - self.get_total_ios()
        total_usec = brstat.get_total_usec() - self.get_total_usec()
        return 0 if total_ios <= 0 else total_usec / total_ios

    def get_read_latency(self, brstat):
        read_ios = brstat.readIos - self.readIos
        read_usec = brstat.readUsec - self.readUsec
        return 0 if read_ios <= 0 else read_usec / read_ios

    def get_write_latency(self, brstat):
        write_ios = brstat.writeIos - self.writeIos
        write_usec = brstat.writeUsec - self.writeUsec
        return 0 if write_ios <= 0 else write_usec / write_ios

    def get_useful_busy_ns(self, brstat):
        return brstat.usefulBusyNs - self.usefulBusyNs

    def get_useful_idle_ns(self, brstat):
        return brstat.usefulIdleNs - self.usefulIdleNs

    def get_useful_cpu_ratio(self, brstat):
        """Hypernode-style reactor utilization: busy / (busy + idle+overhead).

        Reports the fraction of pinned reactor CPU actually spent doing I/O
        work. Independent of Linux mpstat, which shows busy-poll cores at 100%
        regardless of whether they process anything.
        """
        busy = self.get_useful_busy_ns(brstat)
        idle = self.get_useful_idle_ns(brstat)
        total = busy + idle
        return 0.0 if total <= 0 else busy / total

    def get_net_ios(self):
        return self.sendIos + self.recvIos

    def get_net_bytes(self):
        return self.sendBytes + self.recvBytes

    def get_net_usec(self):
        return self.sendUsec + self.recvUsec

    def get_net_qps(self, brstat):
        total_ios = brstat.get_net_ios() - self.get_net_ios()
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else total_ios / elapsed

    def get_send_qps(self, brstat):
        send_ios = brstat.sendIos - self.sendIos
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else send_ios / elapsed

    def get_recv_qps(self, brstat):
        recv_ios = brstat.recvIos - self.recvIos
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else recv_ios / elapsed

    def get_net_xput(self, brstat):
        if len(self.netXput) > 0:
            return sum(self.netXput)
        total_bytes = brstat.get_net_bytes() - self.get_net_bytes()
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else total_bytes / elapsed

    def get_send_xput(self, brstat):
        send_bytes = brstat.sendBytes - self.sendBytes
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else send_bytes / elapsed

    def get_recv_xput(self, brstat):
        recv_bytes = brstat.recvBytes - self.recvBytes
        elapsed = brstat.ts - self.ts
        return 0 if elapsed <= 0 else recv_bytes / elapsed

    def get_net_latency(self, brstat):
        total_ios = brstat.get_net_ios() - self.get_net_ios()
        total_usec = brstat.get_net_usec() - self.get_net_usec()
        return 0 if total_ios <= 0 else total_usec / total_ios

    def get_send_latency(self, brstat):
        send_ios = brstat.sendIos - self.sendIos
        send_usec = brstat.sendUsec - self.sendUsec
        return 0 if send_ios <= 0 else send_usec / send_ios

    def get_recv_latency(self, brstat):
        recv_ios = brstat.recvIos - self.recvIos
        recv_usec = brstat.recvUsec - self.recvUsec
        return 0 if recv_ios <= 0 else recv_usec / recv_ios

    def get_disk_util(self):
        if len(self.diskUtil) > 0:
            return sum(self.diskUtil) / len(self.diskUtil)
        return 0

    def get_cpu_util(self):
        if len(self.cpuUtil) > 0:
            return sum(self.cpuUtil) / len(self.cpuUtil)
        return 0

    def get_net_util(self):
        if len(self.netUtil) > 0:
            return sum(self.netUtil) / len(self.netUtil)
        return 0

    def get_cpu_util_full(self, brstat):
        """
        Calculate utilization percentages for all components of CPU usage
        """
        utils = {"user": [], "nice": [], "system": [], "iowait": [], "idle": []}
        util = {
            "user": 0.0,
            "nice": 0.0,
            "system": 0.0,
            "iowait": 0.0,
            "idle": 0.0,
            "utilization": 0.0,
            "busy": 0.0,
        }
        n = len(self.cpuStats)
        if n == 0:
            return util
        elapsed = 0
        for then, now in zip(self.cpuStats, brstat.cpuStats):
            tick = now.ticksPerSec
            cpus = now.nCpu
            elapsed = (now.ts - then.ts) / 1e9
            user_sec = (now.user - then.user) / tick / cpus
            nice_sec = (now.nice - then.nice) / tick / cpus
            sys_sec = (now.system - then.system) / tick / cpus
            iowait_sec = (now.iowait - then.iowait) / tick / cpus
            idle_sec = (now.idle - then.idle) / tick / cpus
            if elapsed <= 0 or cpus == 0:
                continue
            utils["user"].append(100 * user_sec / elapsed)
            utils["nice"].append(100 * nice_sec / elapsed)
            utils["system"].append(100 * sys_sec / elapsed)
            utils["iowait"].append(100 * iowait_sec / elapsed)
            utils["idle"].append(100 * idle_sec / elapsed)
        for k in utils.keys():
            # Short runs (warmup < reporting interval) can leave initial_stats
            # without a paired CPU sample, yielding empty utils[k]; treat that
            # as 0 utilization rather than dividing by zero.
            util[k] = sum(utils[k]) / len(utils[k]) if utils[k] else 0.0
        util["busy"] = util["user"] + util["nice"] + util["system"]
        util["utilization"] = 100 - util["idle"]
        return util

    def get_cpu_busy(self, brstat):
        """
        Sums CPU user + system percentages
        """
        if len(self.cpuStats) == 0:
            return 0
        busy_percs = []
        for then, now in zip(self.cpuStats, brstat.cpuStats):
            tick = now.ticksPerSec
            cpus = now.nCpu
            elapsed = now.ts - then.ts
            user_diff = now.user - then.user
            sys_diff = now.system - then.system
            busy_diff = user_diff + sys_diff
            if elapsed <= 0 or cpus == 0:
                busy_percs.append(0)
            busy_perc = busy_diff / elapsed / tick / cpus
            busy_percs.append(busy_perc)
        if len(busy_percs) > 0:
            return 100 * sum(busy_percs) / len(busy_percs)
        return 0


class Node:
    """
    A class for representing hosts running the Halcyon Thrift service
    """

    def __init__(self, hostname, json_text=None, partner=None, runtime_config=None):
        self.hostname = hostname
        # @lint-ignore ASTGREP python/python-dns-deps -- benchmark users supply concrete peer hosts, not service tiers
        self.ip = socket.gethostbyname(self.hostname)
        self.partner = partner
        self.config = None
        self.runtime_config = runtime_config
        if json_text:
            self._parse_config_file(json_text, runtime_config)

    def get_total_files(self):
        if self.config is None:
            return 0
        fl = self.config.fileLayout
        mounts = len(self.config.mountpoints)
        file_count = fl.dirs * fl.filesPerDir * self.config.threadsPerDisk
        return file_count * mounts

    def get_total_bytes(self):
        if self.config is None:
            return 0
        fs = self.config.fileLayout.maxFileSize
        return fs * self.get_total_files()

    def _parse_config_file(self, config, runtime_config):
        keys = set(config.keys())
        diff = set(kRequiredKeys) ^ keys
        if diff != set({}):
            raise HalcyonException(
                HalcyonReturnCode.ConfigurationError, f"missing options {diff}"
            )
        read_ratio = config["read_ratio"]
        write_ratio = config["write_ratio"]
        mount_points = config["mount_points"]
        read_perc = read_ratio / (read_ratio + write_ratio)
        read_sizes = dict_to_IoSizeRanges(config["read_sizes"])
        write_sizes = dict_to_IoSizeRanges(config["write_sizes"])
        merged_sizes = merge_IoSizeRanges(config["read_sizes"], config["write_sizes"])
        file_layout = dict_to_FileLayout(config["file_layout"])
        node_config = self._parse_node_config(config)
        self.config = BenchmarkConfiguration(
            readPercentage=read_perc,
            readSizes=read_sizes,
            writeSizes=write_sizes,
            mergedSizes=merged_sizes,
            mountpoints=mount_points,
            fileLayout=file_layout,
            runtimeConfig=runtime_config,
            nodeConfig=node_config,
        )

    def _parse_node_config(self, config):
        target_host = ""
        pair_role = PairRole.SendNone
        if "target_host" in config:
            target_host = config["target_host"]
        if "pair_role" in config:
            pair_role = PairRole[config["pair_role"]]
        return NodeConfig(partner=target_host, role=pair_role)
