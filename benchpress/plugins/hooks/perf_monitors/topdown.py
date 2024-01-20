#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import os
import re
import signal
import subprocess

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
                f"{self.perfspect_path} does not have perf-collect and perf-postprocess"
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
        os.kill(self.proc.pid, signal.SIGINT)
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
        self.is_zen4 = get_amd_zen_generation(self.cpuinfo) >= 4
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


class ARMPerfUtil(BasePerfUtil):
    def __init__(self, job_uuid, **kwargs):
        super(ARMPerfUtil, self).__init__(
            job_uuid,
            "arm-perf-collector",
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
        TopDown = ARMPerfUtil
    else:
        TopDown = DummyPerfUtil
        logger.warning(
            f"Current we only support NVIDIA ARM processors, got vendor '{vendor2}"
        )
else:
    logger.warning(f"Unsupported CPU vendor '{cpu_vendor}'")
    TopDown = DummyPerfUtil
