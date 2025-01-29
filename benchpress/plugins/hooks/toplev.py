#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import os

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)


DEFAULT_BACKGROUND_DURATION_SECS = "30"
DEFAULT_TOPLEV_OUTPUT_CSV_PATH = "toplev_output.csv"
DEFAULT_OPTIONS = [
    "-l1",  # Level
    "-I100",
    "--summary",
    "-m",  # Extra metrics
    "--no-desc",
    "-x,",
    "-o",
    DEFAULT_TOPLEV_OUTPUT_CSV_PATH,
]

DEFAULT_PATH = ["toplev"]


class Toplev(Hook):
    """Collect `toplev` counters during a job.

    - `hook`:
      `options`:
        `args`: list of str - command line arguments to pass to toplev
        `background_mode`:
          `duration`: int or string
          `stdout_path`: str - file path to write stdout of background cmd
          `stderr_path`: str - file path to write stderr of background cmd
        `binary_path`: str - location of toplev command
    """

    def before_job(self, opts, job):
        if not opts:
            opts = {"args": DEFAULT_OPTIONS}
        if "args" not in opts:
            opts["args"] = DEFAULT_OPTIONS

        benchmark_metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        self.default_stdout_path = os.path.join(
            benchmark_metrics_dir,
            "{}_toplev_hook_output_{}".format(job_name, iteration_num),
        )

        if "background_mode" in opts and opts["background_mode"]:
            self.bg_toplev_proc = self._start_background_toplev(opts)
        else:
            binary_path = job.binary
            job.args = opts["args"] + ["--", binary_path] + job.args
            job.binary = opts.get("binary_path", DEFAULT_PATH)

    def after_job(self, opts, job):
        if self.bg_toplev_proc and self.bg_toplev_proc.poll() is None:
            self.bg_toplev_proc.terminate()
            self.stdout.close()

    def _start_background_toplev(self, opts):
        env = {}
        env["http_proxy"] = "fwdproxy:8080"
        env["https_proxy"] = "fwdproxy:8080"

        bg_opts = opts["background_mode"]
        duration = bg_opts.get("duration", DEFAULT_BACKGROUND_DURATION_SECS)
        stdout_path = bg_opts.get("stdout_path", self.default_stdout_path)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201

        cmd = opts.get("binary_path", DEFAULT_PATH)
        cmd += opts["args"]
        cmd += ["--", "sleep", str(duration)]
        logging.info("Starting background Toplev hook: {}".format(" ".join(cmd)))

        return util.issue_background_command(cmd, self.stdout, self.stdout, env)
