#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

# pyre-unsafe

import os
import threading
import time

from . import logger, Monitor


class Power(Monitor):
    def search_sensors(self):
        """
        Search for power sensors under /sys/class/hwmon/hwmon*
        If "devices/power1_oem_info" exists under /sys/class/hwmon/hwmonN directory, it's a valid power sensor
        In this case, record the info of this sensor with the following fields as a dict:
            - path: the full path of the power sensor (/sys/class/hwmon/hwmonN/device)
            - name: the content of the file /sys/class/hwmon/hwmonN/devices/power1_oem_info
            - original_interval: the content of the file /sys/class/hwmon/hwmonN/devices/power1_average_interval, converted to int
            - interval_min: the content of the file /sys/class/hwmon/hwmonN/devices/power1_average_interval_min, converted to int
            - interval_max: the content of the file /sys/class/hwmon/hwmonN/devices/power1_average_interval_max, converted to int
        Append the dict into a list named "sensors"
        """
        sensors = []
        for hwmon in os.listdir("/sys/class/hwmon/"):
            if os.path.isfile(
                os.path.join("/sys/class/hwmon/", hwmon, "device/power1_oem_info")
            ):
                sensor = {}
                sensor["path"] = os.path.join("/sys/class/hwmon/", hwmon, "device")
                with open(
                    os.path.join(sensor["path"], "power1_oem_info"), "r"
                ) as oem_info:
                    sensor["name"] = oem_info.read().strip()
                with open(
                    os.path.join(sensor["path"], "power1_average_interval"), "r"
                ) as interval:
                    sensor["original_interval"] = int(interval.read().strip())
                with open(
                    os.path.join(sensor["path"], "power1_average_interval_min"), "r"
                ) as min_interval:
                    sensor["interval_min"] = int(min_interval.read().strip())
                with open(
                    os.path.join(sensor["path"], "power1_average_interval_max"), "r"
                ) as max_interval:
                    sensor["interval_max"] = int(max_interval.read().strip())
                sensors.append(sensor)
        return sensors

    def set_sensor_avg_interval(self, sensor: dict, interval_ms: int):
        """
        Set the average interval of the given sensor to the specified value
        """
        # Check if the interval is within the range of supported values.
        # If not, use the closest one.
        if interval_ms < sensor["interval_min"]:
            interval_ms = sensor["interval_min"]
        elif interval_ms > sensor["interval_max"]:
            interval_ms = sensor["interval_max"]

        with open(os.path.join(sensor["path"], "power1_average_interval"), "w") as f:
            f.write(str(interval_ms))

    def get_sensor_avg_power(self, sensor: dict):
        """
        Get the average power of the given sensor, in watts
        Note, the unit of the raw value in power1_average is microWatt
        """
        with open(os.path.join(sensor["path"], "power1_average"), "r") as f:
            try:
                return float(f.read()) / 1e6
            except OSError as e:
                return f"<err{e.errno}>"

    def __init__(self, job_uuid, interval=1.0, sensor_interval_ms=None):
        super(Power, self).__init__(interval, "power", job_uuid)
        self.power_sensors = self.search_sensors()
        if sensor_interval_ms is None:
            sensor_interval_ms = 1000 * self.interval
        for sensor in self.power_sensors:
            self.set_sensor_avg_interval(sensor, sensor_interval_ms)

    def do_collect(self):
        row = {}
        row["timestamp"] = time.strftime("%I:%M:%S %p")
        for sensor in self.power_sensors:
            row[sensor["name"]] = self.get_sensor_avg_power(sensor)
        self.res.append(row)

    def collector(self):
        while self.run_power_collector:
            time.sleep(self.interval)
            self.do_collect()

    def run(self):
        if len(self.power_sensors) == 0:
            logger.info("No supported power sensor detected!")
            return
        self.run_power_collector = True
        self.proc = threading.Thread(target=self.collector, name="power", args=())
        self.proc.start()

    def terminate(self):
        self.run_power_collector = False
        if not hasattr(self, "proc"):
            return
        self.proc.join()
        # Restore sensor intervals to their originals
        for sensor in self.power_sensors:
            self.set_sensor_avg_interval(sensor, sensor["original_interval"])
