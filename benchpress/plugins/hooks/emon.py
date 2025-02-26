#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import os
import shutil

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)


DEFAULT_DELAY_SECS = "5"
DEFAULT_OPTIONS = []
DEFAULT_PATH = ["emon"]


class Emon(Hook):
    """Collect `emon` stats during a job.

    - `hook`:
      `options`:
        `args`: list of str - command line arguments to pass to emon
        'binary': binary path specified manually
        `delay`: int or string; delay before launching emon
        'arch_event_file': arch-specific event file
        'arch_xml': arch-specific xml file
        `stdout_path`: str - file path to write stdout of emon
    """

    def before_job(self, opts, job):
        if not opts:
            opts = {"args": DEFAULT_OPTIONS}
        if "args" not in opts:
            opts["args"] = DEFAULT_OPTIONS

        self.benchmark_metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        self.default_stdout_path = os.path.join(
            self.benchmark_metrics_dir,
            f"{job_name}_emon_hook_output_{iteration_num}.dat",
        )
        self.stdout = None
        self.bg_emon_proc = self._start_background_emon(opts)

    def after_job(self, opts, job):
        if self.bg_emon_proc and self.bg_emon_proc.poll() is None:
            self.bg_emon_proc.terminate()
        if self.stdout:
            self.stdout.close()

    def _start_background_emon(self, opts):
        env = self._get_env()
        if env:
            if self._generate_emon_info(self.benchmark_metrics_dir, env):
                event_file = self._fetch_event_file(self.benchmark_metrics_dir, opts)
                if event_file:
                    logger.info("Emon setup successful")
                    delay = opts.get("delay", DEFAULT_DELAY_SECS)
                    stdout_path = opts.get("stdout_path", self.default_stdout_path)
                    self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201
                    cmd = opts.get("binary", DEFAULT_PATH)
                    cmd += ["-i", event_file, "-s", str(delay)]
                    cmd += opts["args"]
                    logging.info(
                        "Starting background Emon hook: {}".format(" ".join(cmd))
                    )
                    return util.issue_background_command(
                        cmd, self.stdout, self.stdout, env
                    )

    def _get_env(self):
        """Get the environment variable to run emon."""
        env = {}
        try:
            cmd = ["/opt/intel/emon/sep_vars.sh"]
            proc = util.issue_background_command(cmd=cmd, stdout=None, stderr=None)
            proc.wait()
            for line in proc.stdout.read().decode("utf-8").splitlines():
                if "=" in line:
                    items = line.split("=")
                    env[items[0]] = items[1]
        except Exception as e:
            logger.warning(f"Is Emon being installed correctly? ({str(e):s})")
        return env

    def _fetch_event_file(self, dst_dir, opts):
        src_event_file = opts.get("arch_event_file", None)
        src_xml_file = opts.get("arch_xml", None)
        if src_event_file and src_xml_file:
            if os.path.exists(src_event_file) and os.path.exists(src_xml_file):
                dst_event_file = os.path.join(dst_dir, src_event_file.split("/")[-1])
                dst_xml_file = os.path.join(dst_dir, src_xml_file.split("/")[-1])
                shutil.copyfile(src_event_file, dst_event_file)
                shutil.copyfile(src_xml_file, dst_xml_file)
                return dst_event_file
            else:
                logger.error(f"{src_event_file} or {src_xml_file} does not exist!")
        else:
            logger.error("arch_event_file & arch_xml options are required for emon!")
        return None

    def _generate_emon_info(self, dst_dir, env):
        try:
            filename = os.path.join(dst_dir, "emon_v_file")
            with open(filename, "w", encoding="utf-8") as fp:
                cmd = ["emon", "-v"]
                proc = util.issue_background_command(
                    cmd=cmd, stdout=fp, stderr=fp, env=env
                )
                proc.wait()
            filename = os.path.join(dst_dir, "emon_m_file")
            with open(filename, "w", encoding="utf-8") as fp:
                cmd = ["emon", "-M"]
                proc = util.issue_background_command(
                    cmd=cmd, stdout=fp, stderr=fp, env=env
                )
                proc.wait()
            return True
        except Exception as e:
            logger.warning(f"Is Emon being installed correctly? ({str(e):s})")
        return False
