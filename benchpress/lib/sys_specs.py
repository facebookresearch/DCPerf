#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import logging
import os
import shlex
import subprocess

from benchpress.lib import dmidecode


def get_cpu_topology():
    lscpu_p = subprocess.Popen(
        ["lscpu"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (lscpu_data, err) = lscpu_p.communicate()
    lscpu_data = lscpu_data.decode("utf-8").split("\n")

    lscpu_dict = {}
    lscpu_dict["sched_getaffinity"] = list(os.sched_getaffinity(0))
    for cpu_stat in lscpu_data:
        if ":" in cpu_stat:
            stat, val = [stat.strip() for stat in cpu_stat.split(":")][:2]
            if stat == "Flags":
                lscpu_dict[stat] = val.split(" ")
            else:
                lscpu_dict[stat] = val

    return lscpu_dict


def get_numa_topology():
    numa_p = subprocess.Popen(
        ["numactl", "--hardware"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (numa_data, err) = numa_p.communicate()
    numa_data = numa_data.decode("utf-8").split("\n")

    numa_dict = {}
    in_distances_section = False
    distance_headers = []

    for line in numa_data:
        stripped_line = line.strip()

        # Capture node sizes (skip free memory as it's in numastat)
        if "node" in stripped_line and "size:" in stripped_line:
            parts = stripped_line.split()
            numa_dict[f"node_{parts[1]}_size"] = f"{parts[3]} {parts[4]}".strip()

        # Start of distance matrix section
        if stripped_line.startswith("node distances:"):
            in_distances_section = True
            numa_dict["node_distances"] = {}
            continue

        # Parse distance matrix header row (node numbers)
        if in_distances_section and stripped_line.startswith("node"):
            distance_headers = stripped_line.split()[
                1:
            ]  # Skip "node" label, get numbers
            continue

        # Parse distance matrix data rows
        if in_distances_section and distance_headers:
            parts = stripped_line.split(":")
            if len(parts) == 2:
                source_node = parts[0].strip()
                distances = parts[1].strip().split()

                # Create entries like "0->0": 10, "0->1": 20
                for i, dist in enumerate(distances):
                    if i < len(distance_headers):
                        target_node = distance_headers[i]
                        key = f"{source_node}->{target_node}"
                        try:
                            numa_dict["node_distances"][key] = int(dist)
                        except ValueError:
                            numa_dict["node_distances"][key] = dist

    return numa_dict


def get_os_kernel():
    sys_name, node_name, kernel_release, version, machine = os.uname()
    return {
        "sys_name": sys_name,
        "node_name": node_name,
        "kernel_release": kernel_release,
        "version": version,
        "machine": machine,
    }


def get_kernel_cmdline():
    if not os.path.exists("/proc/cmdline"):
        return []
    with open("/proc/cmdline", "r") as f:
        kern_cmdline = f.read()
    return shlex.split(kern_cmdline)


def get_dmidecode_data():
    dmidecode_data = dmidecode.parse()
    return dmidecode_data


def get_sysctl_data():
    sysctl_p = subprocess.Popen(
        ["sysctl", "-a"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (kernel_params, err) = sysctl_p.communicate()  # No timeout needed
    kernel_params = kernel_params.decode("utf-8").split("\n")  # Clean up output

    kernel_params_dict = {}
    for kernel_param in kernel_params:
        if "=" in kernel_param:
            param, param_val = [param.strip() for param in kernel_param.split("=")]
            kernel_params_dict[param] = param_val

    return kernel_params_dict


def get_rpm_packages():
    rpm_p = subprocess.Popen(
        ["rpm", "-qa"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (rpm_packages, err) = rpm_p.communicate()
    rpm_packages = [
        rpm_package.strip() for rpm_package in rpm_packages.decode("utf-8").split("\n")
    ]  # Clean up output

    return rpm_packages


def get_dpkg_packages():
    dpkg_p = subprocess.Popen(
        [
            "dpkg-query",
            "--show",
            "--showformat",
            "${Package}-${Version}.${Architecture}\\n",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    (dpkg_packages, err) = dpkg_p.communicate()
    dpkg_packages = [
        dpkg_package.strip()
        for dpkg_package in dpkg_packages.decode("utf-8").split("\n")
    ]  # Clean up output

    return dpkg_packages


def get_cpu_mem_data():
    cpu_mem_p = subprocess.Popen(
        ["cat", "/proc/meminfo"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (cpu_mem_data, err) = cpu_mem_p.communicate()
    cpu_mem_data = cpu_mem_data.decode("utf-8").split("\n")  # Clean up output

    cpu_mem_dict = {}
    for mem_stat in cpu_mem_data:
        if ":" in mem_stat:
            mem, stat = [mem.strip() for mem in mem_stat.split(":")]
            if (
                " " in stat
            ):  # Change units of CPU mem due to some legacy issue in RedHat
                stat = stat.split(" ")
                stat[1] = "KiB"
                stat = " ".join(stat)
            cpu_mem_dict[mem] = stat

    return cpu_mem_dict


def get_hw_data():
    hw_p = subprocess.Popen(
        ["lshw", "-json"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (hw_data, err) = hw_p.communicate()
    try:
        hw_data = json.loads(hw_data.decode("utf-8"))
    except json.decoder.JSONDecodeError:
        logging.warning("Failed to parse output from lshw -json; Skipping it")
        return {}
    return hw_data


def get_os_release_data():
    os_release_p = subprocess.Popen(
        ["cat", "/etc/os-release"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (os_data, err) = os_release_p.communicate()
    os_release_data = os_data.decode("utf-8").split("\n")

    os_release_data_dict = {}
    for os_info in os_release_data:
        if "=" in os_info:
            param, param_val = [param.strip() for param in os_info.split("=")]
            param = param.lower()
            param_val = param_val.replace('"', "")
            os_release_data_dict[param] = param_val

    return os_release_data_dict


def get_numastat():
    numastat_p = subprocess.Popen(
        ["numastat", "-m"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (numastat_data, err) = numastat_p.communicate()
    numastat_data = numastat_data.decode("utf-8").split("\n")

    numastat_dict = {}
    headers = []
    found_headers = False

    for line in numastat_data:
        line = line.strip()
        if not line:
            continue

        # Skip the first line (title line)
        if "Per-node system memory usage" in line:
            continue

        # Look for the header line with Node names and Total
        if not found_headers and ("Node" in line or "Total" in line):
            # Parse headers by combining "Node" with following numbers
            parts = line.split()
            headers = []
            i = 0
            while i < len(parts):
                if parts[i] == "Node" and i + 1 < len(parts) and parts[i + 1].isdigit():
                    headers.append(f"Node {parts[i + 1]}")
                    i += 2
                elif parts[i] == "Total":
                    headers.append("Total")
                    i += 1
                else:
                    i += 1

            # Initialize dictionaries for each node/total
            for header in headers:
                numastat_dict[header] = {}
            found_headers = True
            continue

        # Skip separator lines (lines with dashes)
        if found_headers and "-" in line:
            continue

        # Parse data lines
        if found_headers:
            parts = line.split()
            if len(parts) >= 2:
                metric_name = parts[0]
                values = parts[1:]

                # Assign values to each node/total
                for i, value in enumerate(values):
                    if i < len(headers):  # Make sure we don't go out of bounds
                        header = headers[i]
                        try:
                            numastat_dict[header][metric_name] = float(value)
                        except ValueError:
                            # If conversion to float fails, store as string
                            numastat_dict[header][metric_name] = value

    return numastat_dict


def get_ulimit():
    ulimit_p = subprocess.Popen(
        "ulimit -a", stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True
    )
    (ulimit_data, err) = ulimit_p.communicate()
    ulimit_data = ulimit_data.decode("utf-8").split("\n")

    ulimit_dict = {}

    for line in ulimit_data:
        line = line.strip()
        if not line:
            continue

        # Find the last occurrence of parentheses which contains the flag
        last_paren_start = line.rfind("(")
        last_paren_end = line.rfind(")")

        if last_paren_start == -1 or last_paren_end == -1:
            continue

        # Extract the description (everything before the last parentheses)
        description_part = line[:last_paren_start].strip()

        # Extract the flag and unit (inside the last parentheses)
        flag_part = line[last_paren_start + 1 : last_paren_end]
        # The flag is typically the last part after comma and space
        if ", " in flag_part:
            unit_part = flag_part.split(", ")[0]  # Everything before the comma
            flag = flag_part.split(", ")[-1]  # Everything after the comma
            description = f"{description_part} ({unit_part})"
        else:
            flag = flag_part
            description = description_part

        # Extract the value (everything after the last parentheses)
        value_str = line[last_paren_end + 1 :].strip()

        # Try to convert value to appropriate type
        if value_str == "unlimited":
            value = "unlimited"
        else:
            try:
                # Try integer first
                if "." not in value_str:
                    value = int(value_str)
                else:
                    value = float(value_str)
            except ValueError:
                # If conversion fails, keep as string
                value = value_str

        ulimit_dict[description] = {"flag": flag, "value": value}

    return ulimit_dict
