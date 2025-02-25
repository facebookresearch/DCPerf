#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import logging
import re
import shlex
import subprocess

import click
import tabulate

from .command import BenchpressCommand, TABLE_FORMAT

logger = logging.getLogger(__name__)


def run_cmd(cmd):
    p = subprocess.run(shlex.split(cmd), capture_output=True)
    return p.stdout.decode().strip()


def get_cpuinfo():
    result = {}
    lscpu = run_cmd("lscpu")
    for line in lscpu.splitlines():
        if ":" in line:
            key, value = line.split(":", maxsplit=1)
            result[key.strip()] = value.strip()
    return result


class SystemCheckCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser(
            "system_check",
            help="system_check is a subcommand that can check a series of system configurations, provide a brief report and provide suggestions",
        )
        parser.set_defaults(command=self)

    def system_software(self):
        table = []
        click.echo("**** System Software ****")
        bios_version = run_cmd("dmidecode -s bios-version")
        table.append(["BIOS Version", bios_version])
        bios_release_date = run_cmd("dmidecode -s bios-release-date")
        table.append(["BIOS Release Date", bios_release_date])
        nic = run_cmd("lshw -c network")
        match_venfor = re.search(r"vendor: (.+)", nic)
        if match_venfor:
            nic_vendor = match_venfor.group(1)
            table.append(["NIC Vendor", nic_vendor])

        match_product = re.search(r"product: (.+)", nic)
        if match_product:
            nic_product = match_product.group(1)
            table.append(["NIC Product", nic_product])

        match_firmeware = re.search(r"firmware=([0-9.]+)", nic)
        if match_firmeware:
            nic_firmware = match_firmeware.group(1)
            table.append(["NIC Firmware", nic_firmware])

        bmc_firmware = run_cmd("ipmitool mc info")
        match = re.search(r"Firmware Revision\s+: (\d+\.\d+)", bmc_firmware)
        if match:
            bmc_firmware = match.group(1)
            table.append(["BMC Firmware", bmc_firmware])
        click.echo(tabulate.tabulate(table, tablefmt=TABLE_FORMAT))

    def kernel_config(self):
        table = []
        click.echo("**** Kernel Configurations ****")
        kernel_version = run_cmd("uname -r")
        table.append(["Kernel Version", kernel_version])
        setlinux_status = run_cmd("getenforce")
        row = ["SELinux Status", setlinux_status, ""]
        if setlinux_status == "Disabled":
            row[-1] = click.style("[OK]", fg="green")
        else:
            row[-1] = click.style("[BAD]", fg="red")
        table.append(row)

        nvme_tcp = subprocess.run(shlex.split("modinfo nvme-tcp"), capture_output=True)
        row = ["NVME-TCP Module", "Not Installed", ""]
        if nvme_tcp.returncode != 0:
            row[-1] = click.style("[Bad for SparkBench]", fg="red")
        else:
            row[-1] = click.style("[OK]", fg="green")
            row[-2] = "Present"
        table.append(row)

        open_files_limit = run_cmd("ulimit -n")
        row = ["Open Files Limit", open_files_limit, ""]
        if int(open_files_limit) < 65535:
            row[-1] = click.style("[BAD]", fg="red")
        else:
            row[-1] = click.style("[OK]", fg="green")
        table.append(row)

        thp_status = (
            run_cmd("cat /sys/kernel/mm/transparent_hugepage/enabled")
            .split("[")[1]
            .split("]")[0]
        )
        table.append(["THP Status", thp_status])
        click.echo(tabulate.tabulate(table, tablefmt=TABLE_FORMAT))

    def hardware_config(self):
        table = []
        click.echo("**** Hardware Configurations ****")
        cpuinfo = get_cpuinfo()
        numa_nodes = cpuinfo["NUMA node(s)"]
        table.append(["NUMA Nodes", numa_nodes])

        lscpu = run_cmd("lscpu")
        matches = re.findall(r"NUMA node\d CPU\(s\):\s+(\d+)", lscpu)
        if len(matches) == int(numa_nodes):
            table.append(["CXL", "Not Present"])
        else:
            table.append(["CXL", "Present"])

        arch = cpuinfo["Architecture"]
        if arch == "x86_64":
            boost_status = run_cmd("cat /sys/devices/system/cpu/cpufreq/boost")
            row = ["Boost Status", "", ""]
            if boost_status == "0":
                row[-1] = click.style("[BAD]", fg="red")
                row[-2] = "Disabled"
            else:
                row[-1] = click.style("[OK]", fg="green")
                row[-2] = "Enabled"
            table.append(row)

        vendor_id = cpuinfo["Vendor ID"].lower()
        if "amd" in vendor_id:
            model_name = cpuinfo["Model name"]
            match = re.search(r"AMD EPYC (\w{4,5})", model_name)
            if match:
                model_num = match.group(1)
                if int(model_num[3]) == 3:
                    uefi = 'uefisettings get "Determinism Slider"'
                else:
                    uefi = 'uefisettings get "Determinism Enable"'
                uefi = run_cmd(uefi)
                match = re.search(r"answer:\s+(.+),", uefi)
                if match:
                    table.append(["Determinism", match.group(1)])

        memory_speed = run_cmd("dmidecode -t memory")
        match = re.search(r"Speed: (\d+ MT/s)", memory_speed)
        if match:
            speed = match.group(1)
            table.append(["Memory Speed", speed])

        base_frequency = run_cmd("dmidecode -t processor")
        match = re.search(r"Current Speed: (\d+ MHz)", base_frequency)
        if match:
            base_frequency = match.group(1)
            table.append(["Base Frequency", base_frequency])

        threads_per_core = cpuinfo["Thread(s) per core"]
        if int(threads_per_core) >= 2:
            table.append(["SMT", "Enabled"])
        else:
            table.append(["SMT", "Disabled"])

        click.echo(tabulate.tabulate(table, tablefmt=TABLE_FORMAT))

    def run(self, args, jobs):
        self.system_software()
        self.kernel_config()
        self.hardware_config()
