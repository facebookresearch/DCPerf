#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import collections
import logging
import re
import subprocess
import typing


_HANDLE_RE = re.compile(r"^Handle\s+(.+),\s+DMI\s+type\s+(\d+),\s+(\d+)\s+bytes$")
_RECORD_KV_RE = re.compile(r"^\t(.+):\s+(.+)$")
_RECORD_LIST_RE = re.compile(r"^\t{1}(.+):$")
_LIST_ELEMENT_RE = re.compile(r"^\t\t(.+)$")

LOGGER = logging.getLogger(__name__)
LOGGER.setLevel(logging.INFO)

DMI_TYPES = {
    0: "BIOS",
    1: "System",
    2: "Baseboard",
    3: "Chassis",
    4: "Processor",
    5: "Memory Controller",
    6: "Memory Module",
    7: "Cache",
    8: "Port Connector",
    9: "System Slots",
    10: "On Board Devices",
    11: "OEM Strings",
    12: "System Configuration Options",
    13: "BIOS Language",
    14: "Group Associations",
    15: "System Event Log",
    16: "Physical Memory Array",
    17: "Memory Device",
    18: "32-bit Memory Error",
    19: "Memory Array Mapped Address",
    20: "Memory Device Mapped Address",
    21: "Built-in Pointing Device",
    22: "Portable Battery",
    23: "System Reset",
    24: "Hardware Security",
    25: "System Power Controls",
    26: "Voltage Probe",
    27: "Cooling Device",
    28: "Temperature Probe",
    29: "Electrical Current Probe",
    30: "Out-of-band Remote Access",
    31: "Boot Integrity Services",
    32: "System Boot",
    33: "64-bit Memory Error",
    34: "Management Device",
    35: "Management Device Component",
    36: "Management Device Threshold Data",
    37: "Memory Channel",
    38: "IPMI Device",
    39: "Power Supply",
    40: "Additional Information",
    41: "Onboard Devices Extended Information",
    42: "Management Controller Host Interface",
}


def parse():
    try:
        output = _read_dmidecode()
        return _parse_dmidecode(output)
    except Exception:
        return {}


def _read_dmidecode() -> str:
    try:
        output = subprocess.check_output("dmidecode", shell=True)
    except Exception as e:
        LOGGER.error(e)
        raise e
    return output.decode("utf-8")


def _parse_dmidecode(dmidecode_output: str):
    dmihandle_records = dmidecode_output.split("\n\n")
    dmidecode_result = collections.OrderedDict()
    for record in dmihandle_records:
        fields = record.splitlines()
        if len(fields) <= 2:
            continue
        dmihandle_matches = _HANDLE_RE.findall(fields[0])
        if not dmihandle_matches:
            continue
        dmihandle = dmihandle_matches[0]
        dmi_type = int(dmihandle[1])
        if dmi_type not in DMI_TYPES:
            LOGGER.debug('Unrecognized DMI Type "%d"' % dmi_type)
            continue
        dmi_type_str = DMI_TYPES[dmi_type]
        dmihandle_data = _parse_dmihandle_record(fields)
        if dmi_type_str not in dmidecode_result:
            dmidecode_result[dmi_type_str] = [dmihandle_data]
        else:
            dmidecode_result[dmi_type_str].append(dmihandle_data)
    return dmidecode_result


def _parse_dmihandle_record(
    dmihandle_lines: list[str],
) -> dict[str, typing.Any]:
    dmihandle_data = collections.OrderedDict()
    dmihandle_data["_title"] = dmihandle_lines[1]
    list_acc = []
    list_acc_name = ""
    in_list_state = False
    for line in dmihandle_lines[2:]:
        kv_record_match = _RECORD_KV_RE.findall(line)
        list_record_match = _RECORD_LIST_RE.findall(line)
        list_element_match = _LIST_ELEMENT_RE.findall(line)

        if in_list_state:
            if not list_element_match or kv_record_match or list_record_match:
                dmihandle_data[list_acc_name] = list_acc
                in_list_state = False
                list_acc_name = ""
                list_acc = []
            else:  # Must be a list element
                list_element = list_element_match[0].strip()
                list_acc.append(list_element)
        else:
            if kv_record_match:
                kv_record_field = kv_record_match[0]
                dmihandle_data[kv_record_field[0]] = kv_record_field[1]
            elif list_record_match:
                list_record_field = list_record_match[0]
                list_acc_name = list_record_field.strip()
                list_acc = []
                in_list_state = True
    return dmihandle_data
