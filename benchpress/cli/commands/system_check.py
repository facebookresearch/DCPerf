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
import sys
from dataclasses import dataclass
from typing import Any

import click
import tabulate
import yaml
from packaging.version import Version

from .command import BenchpressCommand, TABLE_FORMAT

logger = logging.getLogger(__name__)


@dataclass
class Result:
    value: Any
    failed: bool
    failure_reason: str
    expected: Any = None


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
    def __init__(self):
        self.run_cmd = run_cmd
        self.fbk_version_regex: str = r"(\d+\.\d+\.\d+-\d+)_fbk(\d+)"

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
        bios_version = self.run_cmd("dmidecode -s bios-version")
        table.append(["BIOS Version", bios_version])
        bios_release_date = self.run_cmd("dmidecode -s bios-release-date")
        table.append(["BIOS Release Date", bios_release_date])
        nic = self.run_cmd("lshw -c network")
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

        bmc_firmware = self.run_cmd("ipmitool mc info")
        match = re.search(r"Firmware Revision\s+: (\d+\.\d+)", bmc_firmware)
        if match:
            bmc_firmware = match.group(1)
            table.append(["BMC Firmware", bmc_firmware])
        click.echo(tabulate.tabulate(table, tablefmt=TABLE_FORMAT))

    def kernel_config(self):
        table = []
        click.echo("**** Kernel Configurations ****")
        kernel_version = self.run_cmd("uname -r")
        table.append(["Kernel Version", kernel_version])
        setlinux_status = self.run_cmd("getenforce")
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

        open_files_limit = self.run_cmd("ulimit -n")
        row = ["Open Files Limit", open_files_limit, ""]
        if int(open_files_limit) < 65535:
            row[-1] = click.style("[BAD]", fg="red")
        else:
            row[-1] = click.style("[OK]", fg="green")
        table.append(row)

        thp_status = (
            self.run_cmd("cat /sys/kernel/mm/transparent_hugepage/enabled")
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

        lscpu = self.run_cmd("lscpu")
        matches = re.findall(r"NUMA node\d CPU\(s\):\s+(\d+)", lscpu)
        if len(matches) == int(numa_nodes):
            table.append(["CXL", "Not Present"])
        else:
            table.append(["CXL", "Present"])

        arch = cpuinfo["Architecture"]
        if arch == "x86_64":
            boost_status = self.run_cmd("cat /sys/devices/system/cpu/cpufreq/boost")
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
                uefi = self.run_cmd(uefi)
                match = re.search(r"answer:\s+(.+),", uefi)
                if match:
                    table.append(["Determinism", match.group(1)])

        memory_speed = self.run_cmd("dmidecode -t memory")
        match = re.search(r"Speed: (\d+ MT/s)", memory_speed)
        if match:
            speed = match.group(1)
            table.append(["Memory Speed", speed])

        base_frequency = self.run_cmd("dmidecode -t processor")
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

    def success(self, name: str, value: Any, expected: Any = None) -> None:
        desc = f"{name:50s}{value}"
        if expected:
            desc += f" (expected {expected})"

        click.echo(click.style("[OK]    ", fg="green") + desc)

    def fail(self, name, value, expected, match_type):
        click.echo(
            click.style(
                "[BAD]   ",
                fg="red",
            )
            + f"{name:50s}"
            + f"Mismatch, got `{value}', exp `{expected}' (type={match_type})"
        )

    def skip(self, name, msg):
        click.echo(click.style("[SKIP]  ", fg="yellow") + f"{name:50s}{msg}")

    def warn(self, msg):
        click.echo(click.style(f"{msg}", fg="yellow"))

    def fatal(self, msg):
        logger.error(msg)
        sys.exit(1)

    def fbk_to_version(self, fbk_str: str) -> Result:
        result = Result("", False, "")
        version_str = ""

        match = re.search(self.fbk_version_regex, fbk_str)
        if match:
            version = match.group(0)
            version_str += version.replace("-", ".").replace("_fbk", ".")
            result.value = Version(version_str)
        else:
            result.failed = True
            result.failure_reason = f"Could not parse fbk string `{fbk_str}`"

        return result

    def version_to_fbk(self, version: Version) -> str:
        assert isinstance(version, Version), "version must be a Version object"
        assert (
            len(str(version)) == 4
        ), "fbk version must be a Version object with 5 parts"

        version_toks = str(version).split(".")

        fbk_str = ".".join(version_toks[:3])
        fbk_str += f"-{version_toks[3]}"
        fbk_str += f"_fbk{version_toks[4]}"

        return fbk_str

    def parse_value_with_rule(self, value, parse_rule) -> Result:
        result = Result("", True, "Unknown failure reason")

        if parse_rule == "":
            result.value = value
            result.failed = False
        elif parse_rule == "fbk":
            result = self.fbk_to_version(value)
        elif parse_rule == "hex":
            try:
                result.value = str(int(value, 16))
                result.failed = False
            except ValueError:
                result.failure_reason = f"Could not parse value `{value}` as hex"
        elif parse_rule.startswith("regex:"):
            regex = parse_rule[len("regex:") :]
            match = re.search(regex, value)

            if match:
                # Check if "parsed" is present in the match object
                if "parsed" in match.groupdict():
                    parsed_value = match.group("parsed")

                    result.value = parsed_value.strip()
                    result.failed = False
                else:
                    result.failure_reason = f"Match regex matched, but no `parsed` group found in regex `{regex}`"
            else:
                result.failure_reason = (
                    f"No match found for regex `{regex}` in value `{value}`"
                )

        else:
            result.failure_reason = f"Unknown parse rule `{parse_rule}`"

        if not result.failed:
            result.failure_reason = ""

        return result

    def match_value_with_rule(self, parsed_value, expected_value, match_rule) -> Result:
        result = Result("", True, "Unknown failure reason")

        if match_rule == "semantic":
            parsed_version_number = Version(str(parsed_value))
            expected_version_number = Version(str(expected_value))

            if parsed_version_number >= expected_version_number:
                result.failed = False
                result.value = parsed_version_number
            else:
                result.failed = True
                result.value = parsed_version_number
                result.failure_reason = f"Expected version number >= {expected_version_number}, but got {parsed_version_number}"
        else:
            result.failed = True
            result.failure_reason = f"Unknown match rule `{match_rule}`"

        return result

    def handle_validation_failure(
        self, args, check, value_found, expected_value
    ) -> bool:
        fixes_available: bool = False

        self.fail(check["name"], value_found, expected_value, check["match_type"])

        if args.run_fixes:
            if "fix" in check:
                self.warn(f"\tFixing {check['name']} with `{check['fix']}`")
                self.run_cmd(check["fix"])
        elif "fix" in check:
            fixes_available = True

        return fixes_available

    def validate_system_serf(self, check, ignore_error) -> Result:
        result_raw = self.run_cmd(
            "serf get $(hostname) --fields '" + check["fields"] + "' --format json"
        )

        result = Result("<no match>", True, "", check["value"])
        check["match_type"] = "serf"

        # Make sure check's fields are all present and valid
        if ("value_parse_rule" in check) ^ ("value_match_rule" in check):
            raise Exception(
                "Either both or neither of value_parse_rule and value_match_rule should be present"
            )
        if "key" not in check:
            self.fatal("key field is required for serf check")
        if "value" not in check:
            self.fatal("value field is required for serf check")
        if "fields" not in check:
            self.fatal("fields field is required for serf check")
        if "selectors" not in check:
            self.fatal("selectors field is required for serf check")

        selectors = check["selectors"]
        assert len(selectors) > 0

        try:
            result_json = json.loads(result_raw)
        except json.JSONDecodeError as e:
            self.warn(f"serf get failed: {e}")
            self.warn(f"Input JSON: {result_raw}")
            result.failed = True
            result.value = "<not present>"
            self.fatal("Could not parse serf result as JSON")

        for item in result_json:
            match: bool = True
            for selector in selectors:
                if not item[selector["key"]] == selector["value"]:
                    match = False

            if match:
                result.value = item[check["key"]]

                if "value_parse_rule" in check:
                    parse_rule_name = check["value_parse_rule"].split(":")[0]

                    check["match_type"] += (
                        f", parse={parse_rule_name}, match={check['value_match_rule']}"
                    )

                    parser_result = self.parse_value_with_rule(
                        result.value, check["value_parse_rule"]
                    )
                    expected_result = self.parse_value_with_rule(
                        check["value"], check["value_parse_rule"]
                    )

                    if parser_result.failed:
                        result.failed = True
                        result.failure_reason = f"Failed to parse value `{result.value}` with rule `{check['value_parse_rule']}`: {parser_result.failure_reason}"
                    elif expected_result.failed:
                        result.failed = True
                        result.failure_reason = f"Failed to parse expected value `{check['value']}` with rule `{check['value_parse_rule']}`: {expected_result.failure_reason}"
                    else:
                        result.expected = expected_result.value
                        match_result = self.match_value_with_rule(
                            parser_result.value,
                            expected_result.value,
                            check["value_match_rule"],
                        )
                        if match_result.failed:
                            result.value = match_result.value
                            result.failed = match_result.failed
                            result.failed = True
                        else:
                            result.failed = False
                            result.value = match_result.value
                else:
                    result.expected = check["value"]
                    result.failed = result.value != check["value"]
                    if result.failed:
                        result.failure_reason = f"Expected value `{check['value']}` but got `{result.value}`"
                break
        if result.failed and result.failure_reason == "":
            result.failure_reason = (
                f"No match found for selectors `{selectors}` in serf result"
            )

        return result

    def validate_system_shell(self, check, ignore_error: bool) -> Result:
        run_cmd_result = self.run_cmd(check["command"], ignore_error).split("\n")[0]
        value_found = run_cmd_result
        expected_value = check["value"]
        failed = False
        failure_reason = ""

        # We need to parse the value and the expected value if a parse rule is present
        # Before we match them
        if "parse_rule" in check:
            assert (
                check["match_type"] == "semantic"
            ), "Currently only semantic match is supported with parse rules"

            parser_result = self.parse_value_with_rule(value_found, check["parse_rule"])
            if parser_result.failed:
                failed = True
                failure_reason = f"Failed to parse value `{value_found}` with rule `{check['parse_rule']}`: {parser_result.failure_reason}"
            else:
                value_found = parser_result.value

            parser_result = self.parse_value_with_rule(
                check["value"], check["parse_rule"]
            )
            if parser_result.failed:
                failed = True
                failure_reason = f"Failed to parse expected value `{check['value']}` with rule `{check['parse_rule']}`: {parser_result.failure_reason}"
                expected_value = "<parse error>"
            else:
                expected_value = parser_result.value

        # Now we can match the values
        if not failed:
            if check["match_type"] == "semantic":
                failed = value_found < expected_value
            elif check["match_type"] == "ignore":
                failed = False
            elif check["match_type"] == "endswith":
                failed = not value_found.endswith(check["value"])
            elif check["match_type"] == "startswith":
                failed = not value_found.startswith(check["value"])
            elif check["match_type"] == "exact":
                failed = value_found != check["value"]
            elif check["match_type"] == "any_exact":
                failed = value_found not in check["value"]
            elif check["match_type"] == "any_startswith":
                failed = not any(value_found.startswith(v) for v in check["value"])
            elif check["match_type"] == "regex":
                match = re.search(check["regex"], value_found)
                if match:
                    value_found = match.group()
                    failed = match.group() != check["value"]
                else:
                    value_found = "<no match>"
                    failed = True
            else:
                self.fatal(f"Unknown match type: {check['match_type']}")

        return Result(value_found, failed, failure_reason, expected_value)

    def validate_system_eth(self, check, ignore_error: bool) -> Result:
        if "interface" not in check:
            raise Exception("interface is required for eth check")
        if "field" not in check:
            raise Exception("field is required for eth check")
        if "value" not in check:
            raise Exception("value is required for eth check")

        result = Result(None, True, "")

        result_raw = self.run_cmd(
            f"ethtool --json {check['options']} {check['interface']}"
        )
        result_list = []

        try:
            result_list = json.loads(result_raw)
        except json.JSONDecodeError as e:
            self.warn(f"ethtool --json failed: {e}")
            self.warn(f"Input JSON: {result_raw}")
            result.failed = True
            result.value = "<not present>"

        assert len(result_list) == 1
        result.value = result_list[0]

        check["match_type"] = "eth"

        if check["field"] not in result.value:
            result.failed = True
            result.value = "<not present>"
            result.failure_reason = (
                f"Field `{check['field']}` not found in ethtool result"
            )
        else:
            result.value = result.value[check["field"]]

            if "sub-field" in check:
                value_found = result.value[check["sub-field"]]
                result.failed = value_found != check["value"]
                result.expected = check["value"]
            else:
                result.failed = result.value != check["value"]
                result.expected = check["value"]

        return result

    def is_predicate_true(self, check) -> bool:
        if "predicate" in check:
            if "predicate_desc" not in check:
                raise Exception("predicate_desc is required if predicate is present")
            if "predicate_value" not in check:
                raise Exception("predicate_value is required if predicate is present")

            predicate = check["predicate"]
            predicate_result = self.run_cmd(predicate)
            return predicate_result == check["predicate_value"]
        else:
            return True

    def validate_system(self, file, args):
        result = Result("", True, "")

        tests_stats = {
            "passed": 0,
            "failed": 0,
        }

        fixes_are_available: bool = False
        config: str = ""

        click.echo(f"**** Validating from {file} ****")

        try:
            with open(file) as f:
                config = yaml.safe_load(f)
        except Exception as e:
            logger.error(f"Failed to load config file {file}:\n\t{e}")
            sys.exit(1)

        value_found: str = ""
        for check in config:
            ignore_error: bool = False
            if "ignore_error" in check:
                ignore_error = check["ignore_error"]

            if not self.is_predicate_true(check):
                self.skip(
                    check["name"], "Predicate not true: " + check["predicate_desc"]
                )
                continue

            if check["type"] == "serf":
                result = self.validate_system_serf(check, ignore_error)

                # Format the value found for display
                if "value_parse_rule" in check:
                    if check["value_parse_rule"] == "hex":
                        # Version() converts the value to an object, we need to convert it back to a string
                        result_str = str(result.value)
                        expected_result_str = str(result.expected)
                        try:
                            result.value = hex(int(result_str))
                            result.expected = hex(int(expected_result_str))
                        except ValueError:
                            result.value = result_str
                            result.failed = True
                    elif check["value_match_rule"] == "semantic":
                        result.value = str(result.value)
                else:
                    result.value = result.value
            elif check["type"] == "shell":
                result = self.validate_system_shell(check, ignore_error)
            elif check["type"] == "eth":
                result = self.validate_system_eth(check, ignore_error)
            else:
                raise Exception(f"Unknown check type: {check['type']}")

            if result.failed:
                fixes_are_available |= self.handle_validation_failure(
                    args, check, result.value, result.expected
                )
                tests_stats["failed"] += 1
            else:
                self.success(check["name"], result.value, result.expected)
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
