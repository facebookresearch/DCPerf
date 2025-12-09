#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import subprocess
import threading
import time

from . import Monitor


class VMStat(Monitor):
    def __init__(self, interval, job_uuid):
        super(VMStat, self).__init__(interval, "vmstat", job_uuid)
        self.run_collector = False

    def collect_vmstat_snapshot(self):
        """Execute vmstat -s and parse the output into a dictionary"""
        result = {}
        try:
            output = subprocess.check_output(["vmstat", "-s"], encoding="utf-8")
            for line in output.strip().split("\n"):
                line = line.strip()
                if not line:
                    continue

                # Split the line into value and description
                parts = line.split(None, 1)
                if len(parts) < 2:
                    continue

                value_str = parts[0]
                description = parts[1]

                # Try to parse the value as a number
                try:
                    value = int(value_str)
                except ValueError:
                    continue

                # Convert description to a metric key name
                # Remove unit suffixes like "K" and clean up the description
                metric_key = description.replace(" K ", " ").strip()
                metric_key = metric_key.replace(" ", "_").replace("-", "_")

                result[metric_key] = value
        except subprocess.CalledProcessError:
            pass

        return result

    def collect(self):
        """Collect vmstat counter snapshots"""
        while self.run_collector:
            snapshot = self.collect_vmstat_snapshot()
            if snapshot:
                snapshot["timestamp"] = time.strftime("%I:%M:%S %p")
                self.res.append(snapshot)
                self.logfile.write(f"Snapshot at {snapshot['timestamp']}\n")
                for key, value in snapshot.items():
                    if key != "timestamp":
                        self.logfile.write(f"  {key}: {value}\n")
                self.logfile.write("\n")
            time.sleep(self.interval)

    def run(self):
        self.run_collector = True
        self.proc = threading.Thread(target=self.collect, name="vmstat", args=())
        self.proc.start()

    def terminate(self):
        self.run_collector = False
        self.proc.join()
