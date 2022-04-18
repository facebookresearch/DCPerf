#!/usr/bin/env python3
# Copyright (c) 2019-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import json
import logging
import os
import subprocess

from benchpress.lib import dmidecode


def get_cpu_topology():
    lscpu_p = subprocess.Popen(
        ["lscpu"], stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    (lscpu_data, err) = lscpu_p.communicate()
    lscpu_data = lscpu_data.decode("utf-8").split("\n")

    lscpu_dict = {}
    for cpu_stat in lscpu_data:
        if ":" in cpu_stat:
            stat, val = [stat.strip() for stat in cpu_stat.split(":")][:2]
            if stat == "Flags":
                lscpu_dict[stat] = val.split(" ")
            else:
                lscpu_dict[stat] = val

    return lscpu_dict


def get_os_kernel():
    sys_name, node_name, kernel_release, version, machine = os.uname()
    return {
        "sys_name": sys_name,
        "node_name": node_name,
        "kernel_release": kernel_release,
        "version": version,
        "machine": machine,
    }


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
