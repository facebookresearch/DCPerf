#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import concurrent.futures
import json
import logging
import os
import re
import warnings

import requests
from urllib3.exceptions import InsecureRequestWarning

from .constants import DEFAULT_CERT_PATH, WATTS_UNITS

logger = logging.getLogger(__name__)


class BMCClient:
    """Communicates with BMC via Redfish REST API over HTTPS (mTLS).
    Discovers and reads power sensors dynamically.
    """

    def __init__(self, hostname: str, cert_path: str = DEFAULT_CERT_PATH):
        self.hostname = hostname
        self.bmc_hostname = self._construct_bmc_hostname(hostname)
        self.cert_path = cert_path
        self.max_threads = min(4, max(1, os.cpu_count() or 1))
        self.sensors: list[dict] = []

    def _construct_bmc_hostname(self, hostname: str) -> str:
        """Convert server hostname to BMC OOB hostname.
        e.g., rtptest8411.atn3.facebook.com -> rtptest8411-oob.atn3.facebook.com
        """
        server_clean = hostname.replace(".facebook.com", "")
        dot_count = server_clean.count(".")
        if dot_count > 2 or (dot_count == 0 and ".facebook.com" not in hostname):
            raise ValueError(
                f"Invalid server format: {hostname}. "
                f"Expected XX.facebook.com, XX.XX, or XX.XX.XX"
            )
        toks = hostname.split(".")
        return toks[0] + "-oob." + ".".join(toks[1:])

    def get_slot_number(self) -> int:
        """Query Serf for rack_sub_position to determine server slot."""
        from ame.serf.clients.py.serf import Serf3ServiceClient
        from facebook.core_systems.queries.ttypes import Query

        if ".facebook.com" not in self.hostname:
            raise ValueError("Hostname malformed. Needs a .facebook.com suffix.")

        with Serf3ServiceClient() as client:
            query = Query(whereMap={"name": self.hostname})
            servers = client.getDevices(query=query, columns=["rack_sub_position"])
            if len(servers) != 1:
                raise ValueError(
                    f"Could not get slot number for host {self.hostname}: "
                    f"got {len(servers)} results"
                )
            return int(servers[0].rack_sub_position)

    def discover_sensors(self, chassis_sensors_paths, slot=None) -> list[dict]:
        """Discover all power sensors from Redfish chassis sensors endpoint(s).

        Args:
            chassis_sensors_paths: Single path string or list of paths,
                e.g. "/redfish/v1/Chassis/server3/Sensors" or a list of such paths.
            slot: Optional server slot number. When provided, per-slot sensors
                on shared chassis (e.g. CALIBRATED_MEDUSA_MB{N}_*) are filtered
                to only keep the current slot's sensor.

        Returns:
            List of {id: odata_id, name: sensor_name} dicts.
            Also stored in self.sensors.
        """
        if isinstance(chassis_sensors_paths, str):
            chassis_sensors_paths = [chassis_sensors_paths]
        all_sensors = []
        for path in chassis_sensors_paths:
            try:
                sensors = self._discover_from_path(path)
                all_sensors.extend(sensors)
            except Exception as e:
                logger.warning(f"Failed to discover sensors from {path}: {e}")
        if slot is not None:
            all_sensors = self._filter_slot_sensors(all_sensors, slot)
        self.sensors = all_sensors
        logger.info(f"Discovered {len(all_sensors)} power sensors from BMC")
        return all_sensors

    def _filter_slot_sensors(self, sensors, slot):
        """Filter per-slot sensors from shared chassis to keep only this slot.

        Sensors like CALIBRATED_MEDUSA_MB{N}_* are per-slot sensors hosted on
        the shared Medusa Board. We only keep the one matching our slot number.
        Truly shared sensors (48V HSC, fans, NICs, etc.) are kept as-is.
        """
        mb_slot_pattern = re.compile(r"CALIBRATED_MEDUSA_MB(\d+)_")
        filtered = []
        for sensor in sensors:
            m = mb_slot_pattern.match(sensor["name"])
            if m:
                if int(m.group(1)) == slot:
                    filtered.append(sensor)
            else:
                filtered.append(sensor)
        return filtered

    def _discover_from_path(self, chassis_sensors_path: str) -> list[dict]:
        """Discover power sensors from a single Redfish chassis sensors endpoint."""
        data = self._fetch_url(chassis_sensors_path)
        data_json = json.loads(data)
        members = data_json.get("Members", [])

        def fetch_sensor_detail(member):
            odata_id = member["@odata.id"]
            try:
                sensor_data = json.loads(self._fetch_url(odata_id))
                reading_units = sensor_data.get("ReadingUnits", "")
                if self._is_watts_sensor(reading_units):
                    return {
                        "id": odata_id,
                        "name": sensor_data["Name"].replace(" ", "_"),
                    }
            except Exception as e:
                logger.warning(f"Failed to fetch sensor detail {odata_id}: {e}")
            return None

        sensors = []
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self.max_threads
        ) as executor:
            futures = [
                executor.submit(fetch_sensor_detail, member) for member in members
            ]
            for future in concurrent.futures.as_completed(futures):
                result = future.result()
                if result is not None:
                    sensors.append(result)

        return sensors

    def read_sensors(self) -> dict[str, float]:
        """Read current values of all discovered sensors in parallel.

        Returns:
            Dict mapping sensor name to reading in Watts.
        """
        if not self.sensors:
            logger.warning("No power sensors discovered. Nothing to read.")
            return {}

        result = {}

        def fetch_reading(sensor):
            try:
                sensor_data = json.loads(self._fetch_url(sensor["id"]))
                return sensor_data["Name"], float(sensor_data["Reading"])
            except Exception as e:
                logger.warning(f"Failed to read sensor {sensor.get('name', '?')}: {e}")
                return None, None

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=self.max_threads
        ) as executor:
            futures = [
                executor.submit(fetch_reading, sensor) for sensor in self.sensors
            ]
            for future in concurrent.futures.as_completed(futures):
                name, reading = future.result()
                if name is not None and reading is not None:
                    clean_name = name.split("/")[-1].replace(" ", "_")
                    result[clean_name] = reading

        return result

    def _fetch_url(self, subpath: str) -> str:
        """HTTPS GET to BMC with mTLS.

        Args:
            subpath: URL path (e.g., "/redfish/v1/Chassis/server3/Sensors")

        Returns:
            Response body as string.
        """
        if subpath.startswith("/"):
            subpath = subpath[1:]
        url = f"https://{self.bmc_hostname}/{subpath}"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", InsecureRequestWarning)
            response = requests.get(url, cert=self.cert_path, verify=False)
        response.raise_for_status()
        return response.text

    def _is_watts_sensor(self, reading_units: str) -> bool:
        """Check if ReadingUnits indicates Watts (case-insensitive)."""
        return reading_units.strip().lower() in WATTS_UNITS
