#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import logging
import re
import shlex
import subprocess

import click
import tabulate
import yaml

from .command import BenchpressCommand, TABLE_FORMAT

logger = logging.getLogger(__name__)


def run_cmd(cmd, ignore_error=False):
    p = subprocess.run(cmd, capture_output=True, shell=True)
    if p.returncode == 0 or ignore_error:
        return p.stdout.decode().strip()
    else:
        return f"Error: {p.stderr.decode().strip()}"


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
        parser.add_argument(
            "--config",
            type=str,
            help="config file(s) to use, comma separated list, if multiple",
        )

        parser.add_argument(
            "--run-fixes", type=bool, help="config file to use", default=False
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

    def success(self, name, value):
        click.echo(click.style("[OK]    ", fg="green") + f"{name:50s}{value}")

    def fail(self, name, value, expected, match_type):
        click.echo(
            click.style(
                "[BAD]   ",
                fg="red",
            )
            + f"{name:50s}"
            + f"Mismatch `{value}' and `{expected}' (type={match_type})"
        )

    def skip(self, name, msg):
        click.echo(click.style("[SKIP]  ", fg="yellow") + f"{name:50s}{msg}")

    def warn(self, msg):
        click.echo(click.style(f"{msg}", fg="yellow"))

    def handle_validation_failure(self, args, check, value_found) -> bool:
        fixes_available: bool = False

        self.fail(check["name"], value_found, check["value"], check["match_type"])

        if args.run_fixes:
            if "fix" in check:
                self.warn(f"\tFixing {check['name']} with `{check['fix']}`")
                run_cmd(check["fix"])
        elif "fix" in check:
            fixes_available = True

        return fixes_available

    def validate_system_serf(self, check, ignore_error):
        result_raw = run_cmd(
            "serf get $(hostname) --fields '" + check["fields"] + "' --format json"
        )
        value_found = ""
        failed = True

        result_json = json.loads(result_raw)

        for item in result_json:
            if check["key_name"] not in item:
                raise Exception(f"{check['key_name']} not found")

            if item[check["key_name"]] == check["key"]:
                value_found = item[check["value_name"]]
                failed = value_found != check["value"]
                break

        check["match_type"] = "serf"

        return (
            value_found,
            failed,
        )

    def validate_system_shell(self, check, ignore_error):
        result = run_cmd(check["command"], ignore_error).split("\n")[0]
        value_found = result
        failed = False
        if check["match_type"] == "ignore":
            failed = False
        elif check["match_type"] == "endswith":
            failed = not result.endswith(check["value"])
        elif check["match_type"] == "startswith":
            failed = not result.startswith(check["value"])
        elif check["match_type"] == "exact":
            failed = result != check["value"]
        elif check["match_type"] == "any_exact":
            failed = result not in check["value"]
        elif check["match_type"] == "any_startswith":
            failed = not any(result.startswith(v) for v in check["value"])
        elif check["match_type"] == "regex":
            match = re.search(check["regex"], result)
            if match:
                value_found = match.group()
                failed = match.group() != check["value"]
            else:
                value_found = "<no match>"
                failed = True
        else:
            raise Exception(f"Unknown match type: {check['match_type']}")

        return (
            value_found,
            failed,
        )

    def validate_system_eth(self, check, ignore_error):
        if "interface" not in check:
            raise Exception("interface is required for eth check")
        if "field" not in check:
            raise Exception("field is required for eth check")
        if "value" not in check:
            raise Exception("value is required for eth check")

        value_found: str = ""
        failed: bool = False

        result_raw = run_cmd(f"ethtool --json {check['options']} {check['interface']}")
        result_list = json.loads(result_raw)
        assert len(result_list) == 1
        result = result_list[0]

        check["match_type"] = "eth"

        if check["field"] not in result:
            failed = True
            value_found = "<not present>"
        else:
            value_found = result[check["field"]]

            if "sub-field" in check:
                value_found = result[check["field"]][check["sub-field"]]
                failed = value_found != check["value"]
            else:
                failed = result[check["field"]] != check["value"]

        return (
            value_found,
            failed,
        )

    def is_predicate_true(self, check) -> bool:
        if "predicate" in check:
            if "predicate_desc" not in check:
                raise Exception("predicate_desc is required if predicate is present")
            if "predicate_value" not in check:
                raise Exception("predicate_value is required if predicate is present")

            predicate = check["predicate"]
            predicate_result = run_cmd(predicate)
            return predicate_result == check["predicate_value"]
        else:
            return True

    def validate_system(self, file, args):
        tests_stats = {
            "passed": 0,
            "failed": 0,
        }

        fixes_are_available: bool = False
        config: str = ""

        click.echo(f"**** Validating from {file} ****")
        with open(file) as f:
            config = yaml.safe_load(f)

        value_found: str = ""
        for check in config:
            failed: bool = False
            ignore_error: bool = False
            if "ignore_error" in check:
                ignore_error = check["ignore_error"]

            if not self.is_predicate_true(check):
                self.skip(check["name"], check["predicate_desc"])
                continue

            if check["type"] == "serf":
                value_found, failed = self.validate_system_serf(check, ignore_error)
            elif check["type"] == "shell":
                value_found, failed = self.validate_system_shell(check, ignore_error)
            elif check["type"] == "eth":
                value_found, failed = self.validate_system_eth(check, ignore_error)
            else:
                raise Exception(f"Unknown check type: {check['type']}")

            if failed:
                fixes_are_available |= self.handle_validation_failure(
                    args, check, value_found
                )
                tests_stats["failed"] += 1
            else:
                self.success(check["name"], value_found)
                tests_stats["passed"] += 1

        tests_stats["fixes_are_available"] = fixes_are_available

        return tests_stats

    def run(self, args, jobs):
        if args.config:
            for file in args.config.split(","):
                tests_stats = self.validate_system(file, args)

                click.echo(
                    click.style(
                        f"Passed: {tests_stats['passed']}, Failed: {tests_stats['failed']}",
                        fg="green" if tests_stats["failed"] == 0 else "red",
                    )
                )

                if tests_stats["fixes_are_available"]:
                    self.warn(
                        "Fixes are available. Run with --run-fixes to apply them."
                    )

        self.system_software()
        self.kernel_config()
        self.hardware_config()
