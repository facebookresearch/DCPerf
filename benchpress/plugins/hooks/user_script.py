#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import os
import signal

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

DEFAULT_BACKGROUND_DURATION_SECS = "60"


class UserScript(Hook):
    def before_job(self, opts, job):
        if not opts or "hook_path" not in opts:
            raise Exception("Path to hook does not exist")

        if "args" not in opts:
            opts["args"] = []

        hook_name = opts["hook_path"].split(".")[0]
        benchmark_metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        self.default_stdout_path = os.path.join(
            benchmark_metrics_dir,
            "{}_{}_hook_output_{}".format(job_name, hook_name, iteration_num),
        )

        if "background_mode" in opts and opts["background_mode"]:
            self.bg_hook_proc = self._start_background_hook(opts)
        else:
            # TODO: What to run if script is not run in background mode?
            pass

    def after_job(self, opts, job):
        if self.bg_hook_proc and self.bg_hook_proc.poll() is None:
            """Since running a bash script spawns additional child processes,
            kill the entire parent group of the process using an interrupt signal"""
            os.killpg(self.bg_hook_proc.pid, signal.SIGINT)
            self.stdout.close()

    def _start_background_hook(self, opts):
        bg_opts = opts["background_mode"]
        duration = bg_opts.get("duration", DEFAULT_BACKGROUND_DURATION_SECS)
        stdout_path = bg_opts.get("stdout_path", self.default_stdout_path)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201
        hook_path = os.path.join(os.getcwd(), opts["hook_path"])

        cmd = [hook_path]
        cmd += opts["args"]
        cmd += ["-a", "sleep", str(duration)]
        logging.info("Starting background Custom hook: {}".format(" ".join(cmd)))

        return util.issue_background_command(cmd, self.stdout, self.stdout)
