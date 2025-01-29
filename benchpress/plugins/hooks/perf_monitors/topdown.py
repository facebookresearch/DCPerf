#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import os
import re
import subprocess

import numpy as np
import pandas as pd

from . import BP_BASEPATH, logger, Monitor


def get_cpuinfo():
    """
    Catch the output of `lscpu` command and parse it into a dictionary
    """
    try:
        p = subprocess.Popen(
            "lscpu", stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8"
        )
    except OSError as e:
        logger.error("Error while running lscpu: " + str(e))
        return {}
    out, err = p.communicate()
    result = {}
    for line in out.splitlines():
        if ":" in line:
            key, value = line.split(":", maxsplit=1)
            result[key.strip()] = value.strip()
    return result


def get_cpu_vendor(cpuinfo: dict):
    """
    Get the vendor of the CPU based on the output of `lscpu`.
    If "Architecture" is "aarch64", return "arm"
    If "Architecture" is "x86_64", return the vendor (amd/intel) based on "Vendor ID":
        - GenuineIntel -> intel
        - AuthenticAMD -> amd
    """
    arch = cpuinfo["Architecture"]
    if arch == "aarch64":
        return "arm"
    elif arch == "x86_64":
        vendor_id = cpuinfo["Vendor ID"].lower()
        if "intel" in vendor_id:
            return "intel"
        elif "amd" in vendor_id:
            return "amd"
        else:
            logger.warning(f"Unsupported CPU Vendor ID: {vendor_id}")
            return vendor_id
    else:
        logger.warning(f"Unsupported architecture: {arch}")
    return arch


def get_cpu_vendor_from_dmi():
    """
    Get the vendor of the CPU based on the output of `dmidecode -s processor-manufacturer`
    Extract the string until encountering a punctuation, then convert to lower case.
    """
    try:
        p = subprocess.Popen(
            ["dmidecode", "-s", "processor-manufacturer"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
        )
    except OSError as e:
        logger.error("Error while running dmidecode: " + str(e))
        return ""
    out, err = p.communicate()
    out = out.splitlines()[0].strip()
    match = re.search(r"([\w\s]+)", out)
    if match:
        return match.group(1).lower()
    return ""


def get_amd_zen_generation(cpuinfo: dict):
    """
    Get the Zen generation of AMD CPU based on "Model name" of `lscpu` output.
    Assume the model name follows the format of "AMD EPYC <model-number> XX-Core Processor",
    extract the fourth digit of "<model-number>". Model-number can be of 4 or 5 digits and
    may contain numbers or letter "H", "F" or "P".
    """
    model_name = cpuinfo["Model name"]
    match = re.search(r"AMD EPYC (\w{4,5})", model_name)
    if match:
        model_num = match.group(1)
        return int(model_num[3])
    else:
        logger.warning(f"Failed to parse AMD Zen generation from {model_name}")
        return 0


class IntelPerfSpect(Monitor):
    def __init__(self, job_uuid, interval=125, perfspect_path=None):
        super(IntelPerfSpect, self).__init__(interval, "perfspect", job_uuid)
        if perfspect_path is None:
            self.perfspect_path = os.path.join(BP_BASEPATH, "perfspect")
        else:
            self.perfspect_path = perfspect_path
        self.collect_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid, "perf-collect.csv"
        )
        self.postprocess_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid, "topdown-intel.csv"
        )
        if os.path.exists(
            os.path.join(self.perfspect_path, "perf-collect")
        ) and os.path.exists(os.path.join(self.perfspect_path, "perf-postprocess")):
            self.supported = True
        else:
            self.supported = False
            logger.warning(
                f"{self.perfspect_path} does not have perf-collect and perf-postprocess.\n"
                f"Please download PerfSpect and copy the binaries to {self.perfspect_path}."
            )

    def run(self):
        if not self.supported:
            return
        args = [
            os.path.join(self.perfspect_path, "perf-collect"),
            "-m",
            str(self.interval),
            "-o",
            self.collect_output_path,
        ]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8"
        )
        super(IntelPerfSpect, self).run()

    def terminate(self):
        if not self.supported:
            return
        pk = subprocess.Popen(
            ["killall", "-s", "SIGINT", "perf-collect"],
            stdout=self.logfile,
            stderr=self.logfile,
            encoding="utf-8",
        )
        pk.wait()
        exitcode = self.proc.wait()
        if exitcode != 0:
            logger.warning(f"perf-collect failed with exit code {exitcode}")

    def write_csv(self):
        if not self.supported:
            return
        args = [
            os.path.join(self.perfspect_path, "perf-postprocess"),
            "-r",
            self.collect_output_path,
            "-o",
            self.postprocess_output_path,
        ]
        self.proc = subprocess.Popen(
            args, stdout=self.logfile, stderr=self.logfile, encoding="utf-8"
        )
        exitcode = self.proc.wait()
        if exitcode != 0:
            logger.warning(f"perf-postprocess failed with exit code {exitcode}")


class BasePerfUtil(Monitor):
    def __init__(
        self,
        job_uuid,
        name,
        perf_collect_script_name,
        perf_postproc_script_name,
        perfutils_path=None,
        perf_postproc_args=None,
    ):
        super(BasePerfUtil, self).__init__(0, name, job_uuid)
        if perfutils_path is None:
            self.perfutils_path = os.path.join(BP_BASEPATH, "perfutils")
        else:
            self.perfutil_path = perfutils_path
        self.perf_collect_script_name = perf_collect_script_name
        self.perf_postproc_script_name = perf_postproc_script_name
        self.perf_postproc_addl_args = (
            list(perf_postproc_args) if perf_postproc_args else []
        )
        self.postprocess_timeseries_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid, f"{name}-timeseries.csv"
        )
        self.postprocess_summary_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid, f"{name}-summary.csv"
        )

    def run(self):
        perf_collect_script = os.path.join(
            self.perfutils_path, self.perf_collect_script_name
        )
        if not os.path.exists(perf_collect_script):
            logger.warning(f"{perf_collect_script} does not exist")
            return
        cmd = [perf_collect_script]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, encoding="utf-8")
        super(BasePerfUtil, self).run()

    def gen_csv(self):
        pass

    def write_csv(self):
        postproc_script = os.path.join(
            self.perfutils_path, self.perf_postproc_script_name
        )
        if not os.path.exists(postproc_script):
            logger.warning(f"{postproc_script} does not exist, could not postprocess")
            return
        postproc_cmd = (
            [
                postproc_script,
                "-s",
                self.postprocess_timeseries_output_path,
                "-f",
                "csv",
            ]
            + self.perf_postproc_addl_args
            + [self.logpath]
        )

        with open(self.postprocess_summary_output_path, "w") as summary_file:
            self.proc = subprocess.Popen(
                postproc_cmd,
                stdout=summary_file,
                stderr=subprocess.PIPE,
                encoding="utf-8",
            )
        exitcode = self.proc.wait()
        logger.warning(self.proc.stderr.read())
        if exitcode != 0:
            logger.warning(
                f"{self.perf_postproc_script_name} failed with exit code {exitcode}"
            )


class AMDPerfUtil:
    def __init__(self, job_uuid, **kwargs):
        self.cpuinfo = get_cpuinfo()
        self.cpu_vendor = get_cpu_vendor(self.cpuinfo)
        if self.cpu_vendor != "amd":
            raise Exception("Not an AMD processor!")
        if get_amd_zen_generation(self.cpuinfo) >= 4:
            self.is_zen4 = True
        # Provide an user option to override the default behavior of detecting Zen4
        # This is to support engineering sample CPUs who don't have formal CPU model names
        elif "is_zen4" in kwargs and kwargs["is_zen4"]:
            self.is_zen4 = True
        else:
            self.is_zen4 = False
        self.perfutil = BasePerfUtil(
            job_uuid,
            "amd-perf-collector",
            perf_collect_script_name="collect_amd_perf_counter.sh",
            perf_postproc_script_name="generate_amd_perf_report.py",
        )
        if self.is_zen4:
            self.perfutil_zen4 = BasePerfUtil(
                job_uuid,
                "amd-zen4-perf-collector",
                perf_collect_script_name="collect_amd_zen4_perf_counters.sh",
                perf_postproc_script_name="generate_amd_perf_report.py",
                perf_postproc_args=["--arch", "zen4"],
            )

    def run(self):
        self.perfutil.run()
        if self.is_zen4:
            self.perfutil_zen4.run()

    def terminate(self):
        self.perfutil.terminate()
        if self.is_zen4:
            self.perfutil_zen4.terminate()

    def gen_csv(self):
        pass

    def write_csv(self):
        self.perfutil.write_csv()
        if self.is_zen4:
            self.perfutil_zen4.write_csv()


class ARMPerfUtil(Monitor):
    TOPDOWN_TOOL_URL = (
        "https://git.gitlab.arm.com/telemetry-solution/telemetry-solution.git"
    )

    def __init__(self, job_uuid, interval=5):
        super(ARMPerfUtil, self).__init__(interval, "arm-perf-collector", job_uuid)
        self.avail = self.install_if_not_available()
        if not self.avail:
            logger.warning(
                "topdown-tool is not available and we weren't able to install it."
            )
            logger.warning(
                "Please install it manually by following the guide at "
                + "https://learn.arm.com/install-guides/topdown-tool/"
            )

    def install_if_not_available(self):
        try:
            proc = subprocess.run(["topdown-tool", "--help"], capture_output=True)
            if proc.returncode == 0:
                return True
        except FileNotFoundError:
            pass

        logger.warning("ARM topdown-tool is not available, trying to install...")
        try:
            subprocess.run(["git", "clone", self.TOPDOWN_TOOL_URL], check=True)
            os.chdir("telemetry-solution/tools/topdown_tool")
            subprocess.run(["sudo", "pip-3.9", "install", "."], check=True)
            subprocess.run(["topdown-tool", "--help"], capture_output=True, check=True)
        except subprocess.CalledProcessError as e:
            print(
                f"Failed to install topdown-tool. Command: {e.cmd}, exit code: {e.returncode}"
            )
            return False
        except OSError:
            print("Unable to chdir telemetry-solution/tools/topdown_tool")
            return False
        return True

    def run(self):
        if not self.avail:
            return

        self.proc = subprocess.Popen(
            [
                "topdown-tool",
                "-a",
                "-i",
                str(self.interval * 1000),
                "--csv",
                self.csvpath,
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
        )
        super(ARMPerfUtil, self).run()

    def write_csv(self):
        """
        Override the original write_csv() method to write a transposed version
        of CSV table based on what topdown-tool generates
        """
        if not os.path.exists(self.csvpath):
            return
        t_csv_path = self.gen_path(f"{self.name}-transposed.csv")
        df = pd.read_csv(self.csvpath)
        t_rows = []
        for i in range(len(df)):
            metric_key = df.iloc[i]["group"] + "/" + df.iloc[i]["metric"]
            if len(t_rows) == 0 or not np.isclose(
                df.iloc[i]["time"], t_rows[-1]["time"]
            ):
                t_rows.append(
                    {"time": df.iloc[i]["time"], metric_key: df.iloc[i]["value"]}
                )
            else:
                t_rows[-1][metric_key] = df.iloc[i]["value"]
        df_t = pd.DataFrame(t_rows)
        df_t.to_csv(t_csv_path, index=False)


class NVPerfUtil(BasePerfUtil):
    def __init__(self, job_uuid, **kwargs):
        super(NVPerfUtil, self).__init__(
            job_uuid,
            "nv-perf-collector",
            perf_collect_script_name="collect_nvda_neoversev2_perf_counters.sh",
            perf_postproc_script_name="generate_arm_perf_report.py",
        )


class DummyPerfUtil:
    def __init__(self, job_uuid, **kwargs):
        pass

    def run(self):
        logger.warning("TopDown does not work because this CPU is not supported.")

    def terminate(self):
        pass

    def gen_csv(self):
        pass

    def write_csv(self):
        pass


cpuinfo = get_cpuinfo()
cpu_vendor = get_cpu_vendor(cpuinfo)
if cpu_vendor == "intel":
    TopDown = IntelPerfSpect
elif cpu_vendor == "amd":
    TopDown = AMDPerfUtil
elif cpu_vendor == "arm":
    vendor2 = get_cpu_vendor_from_dmi()
    if vendor2 == "nvidia":
        TopDown = NVPerfUtil
    else:
        TopDown = ARMPerfUtil
else:
    logger.warning(f"Unsupported CPU vendor '{cpu_vendor}'")
    TopDown = DummyPerfUtil
