#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import os
import re
import socket
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

    if "Permission denied" in err:
        logger.error("Error with permissions: " + err)
        return ""

    out = out.splitlines()[0].strip()
    match = re.search(r"([\w\s]+)", out)
    if match:
        return match.group(1).lower()
    return ""


def get_amd_zen_generation(cpuinfo: dict):
    """
    Get the Zen generation of AMD CPU based on "Model name" and "CPU family" of `lscpu` output.
    """
    cpu_family = cpuinfo.get("CPU family")
    model_name = cpuinfo.get("Model name")

    if not cpu_family or not model_name:
        logger.warning(f"Failed to parse AMD Zen generation from {cpuinfo}")
        return 0

    zen5_models = [
        "AMD EPYC",
        "100-000000976-14",
        "100-000001458-01",
        "100-000001460-02",
        "100-000001537-04",
        "100-000001463-04",
        "100-000001535-05",
    ]

    if cpu_family == "25" or "zen4" in model_name.lower():
        return "zen4"
    elif cpu_family == "26":
        for model in zen5_models:
            if model in model_name:
                return "zen5"
        return "zen5es"
    else:
        return "zen3"


def get_os_release():
    if not os.path.exists("/etc/os-release"):
        return "Unknown"
    with open("/etc/os-release", "r") as f:
        lines = f.readlines()
    os_release = None
    for line in lines:
        if line.startswith("NAME="):
            os_release = line.strip().split("=")[1].strip('"')
            break
    if "CentOS" in os_release:
        return "CentOS"
    elif "Ubuntu" in os_release:
        return "Ubuntu"
    else:
        return "Unknown"


class IntelPerfSpect(Monitor):
    def __init__(self, job_uuid, mux_interval_msecs=125, perfspect_path=None):
        # PerfSpect 1.x does not support specifying interval
        super(IntelPerfSpect, self).__init__(1, "perfspect", job_uuid)
        self.mux_interval_msecs = mux_interval_msecs
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
            str(self.mux_interval_msecs),
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


class IntelPerfSpect3(Monitor):
    def __init__(
        self,
        job_uuid,
        report_interval_secs=5,
        mux_interval_msecs=125,
        perfspect_path=None,
    ):
        super(IntelPerfSpect3, self).__init__(
            report_interval_secs, "perfspect3", job_uuid
        )
        self.mux_interval_msecs = mux_interval_msecs
        if perfspect_path is None:
            self.perfspect_path = os.path.join(BP_BASEPATH, "perfspect")
        else:
            self.perfspect_path = perfspect_path
        self.collect_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid
        )
        self.postprocess_output_path = os.path.join(
            BP_BASEPATH, "benchmark_metrics_" + self.job_uuid, "topdown-intel.sys.csv"
        )
        if os.path.exists(os.path.join(self.perfspect_path, "perfspect")):
            self.supported = True
        else:
            self.supported = False
            logger.warning(
                f"{self.perfspect_path} does not have perfspect binary.\n"
                f"Please download PerfSpect and extract the `perfspect` executable to {self.perfspect_path}."
            )

    def run(self):
        if not self.supported:
            return
        args = [
            os.path.join(self.perfspect_path, "perfspect"),
            "metrics",
            "--interval",
            str(self.interval),
            "--muxinterval",
            str(self.mux_interval_msecs),
            "--output",
            self.collect_output_path,
        ]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8"
        )
        super(IntelPerfSpect3, self).run()

    def write_csv(self):
        # In PerfSpect 3, there is no need to do post-processing
        # We just rename the output CSV to a standardized name so that PerfPub can easily recognize it
        if not self.supported:
            return
        original_name = os.path.join(
            self.collect_output_path, f"{socket.gethostname()}_metrics.csv"
        )
        new_name = self.postprocess_output_path
        if os.path.exists(original_name):
            os.rename(original_name, new_name)
        else:
            logger.warning(
                f"{original_name} does not exist. was collection successful?"
            )


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
        self.amd_gen = get_amd_zen_generation(self.cpuinfo)
        self.perfutil = BasePerfUtil(
            job_uuid,
            "amd-perf-collector",
            perf_collect_script_name="collect_amd_perf_counters.sh",
            perf_postproc_script_name="generate_amd_perf_report.py",
        )
        if self.amd_gen == "zen4":
            self.perfutil_zen4 = BasePerfUtil(
                job_uuid,
                "amd-zen4-perf-collector",
                perf_collect_script_name="collect_amd_zen4_perf_counters.sh",
                perf_postproc_script_name="generate_amd_perf_report.py",
                perf_postproc_args=["--arch", "zen4"],
            )
        elif self.amd_gen == "zen5":
            self.perfutil = BasePerfUtil(
                job_uuid,
                "amd-zen5-perf-collector",
                perf_collect_script_name="collect_amd_zen5_perf_counters.sh",
                perf_postproc_script_name="generate_amd_perf_report.py",
                perf_postproc_args=["--arch", "zen5"],
            )
        elif self.amd_gen == "zen5es":
            self.perfutil = BasePerfUtil(
                job_uuid,
                "amd-zen5-perf-collector",
                perf_collect_script_name="collect_amd_zen5_perf_counters.sh",
                perf_postproc_script_name="generate_amd_perf_report.py",
                perf_postproc_args=["--arch", "zen5es"],
            )

    def run(self):
        self.perfutil.run()
        if self.amd_gen == "zen4":
            self.perfutil_zen4.run()

    def terminate(self):
        self.perfutil.terminate()
        if self.amd_gen == "zen4":
            self.perfutil_zen4.terminate()

    def gen_csv(self):
        pass

    def write_csv(self):
        self.perfutil.write_csv()
        if self.amd_gen == "zen4":
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
            if get_os_release() == "CentOS":
                subprocess.run(["sudo", "pip-3.9", "install", "."], check=True)
            elif get_os_release() == "Ubuntu":
                subprocess.run(["sudo", "pip3", "install", "."], check=True)
            else:
                logger.warning("Unsupported OS for installing topdown-tool")
                return False
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


def choose_perfspect():
    perfspect_dir = os.path.join(BP_BASEPATH, "perfspect")
    perfspect3_bin = os.path.join(perfspect_dir, "perfspect")
    perfspect1_bin1 = os.path.join(perfspect_dir, "perf-collect")
    perfspect1_bin2 = os.path.join(perfspect_dir, "perf-postprocess")
    if os.path.exists(perfspect3_bin):
        return IntelPerfSpect3
    elif os.path.exists(perfspect1_bin1) and os.path.exists(perfspect1_bin2):
        return IntelPerfSpect
    else:
        return DummyPerfUtil


cpuinfo = get_cpuinfo()
cpu_vendor = get_cpu_vendor(cpuinfo)
if cpu_vendor == "intel":
    TopDown = choose_perfspect()
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
