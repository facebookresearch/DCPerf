#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import threading
import time

from . import logger, Monitor


class NetStat(Monitor):
    def __init__(self, interval, job_uuid, additional_counters=()):
        super(NetStat, self).__init__(interval, "net-stat", job_uuid)
        counters = {"rx_bytes", "rx_packets", "tx_bytes", "tx_packets"}
        self.counters = counters.union(set(additional_counters))
        self.run_collector = False
        try:
            self.interfaces = os.listdir("/sys/class/net")
        except FileNotFoundError:
            logger.warning("/sys/class/net does not exist - will not measure network")
            self.interfaces = []

    def collect_counters(self):
        result = {}
        bad_counters = []
        for netif in self.interfaces:
            result[netif] = {}
            for counter in self.counters:
                fpath = f"/sys/class/net/{netif}/statistics/{counter}"
                if not os.path.exists(fpath):
                    logger.warning("unsupported net counter - " + counter)
                    bad_counters.append(counter)
                    continue
                with open(fpath, "r") as f:
                    value = float(f.read())
                result[netif][counter] = value
        if len(bad_counters) > 0:
            for bad_counter in bad_counters:
                self.counters.remove(bad_counter)
        return result

    def calculate_rates(self, last_dp, this_dp, duration):
        rates = {}
        for netif in self.interfaces:
            for counter in self.counters:
                rate = (this_dp[netif][counter] - last_dp[netif][counter]) / duration
                key = f"{netif}_{counter}_per_sec"
                rates[key] = round(rate, 3)
        return rates

    def collect(self):
        last_ts = time.time()
        last_dp = self.collect_counters()
        while self.run_collector:
            time.sleep(self.interval)
            this_ts = time.time()
            this_dp = self.collect_counters()
            duration = this_ts - last_ts
            rates = self.calculate_rates(last_dp, this_dp, duration)
            rates["timestamp"] = time.strftime("%I:%M:%S %p")
            self.res.append(rates)
            last_dp = this_dp
            last_ts = this_ts

    def run(self):
        self.run_collector = True
        self.proc = threading.Thread(target=self.collect, name="net-stat", args=())
        self.proc.start()

    def terminate(self):
        self.run_collector = False
        self.proc.join()
