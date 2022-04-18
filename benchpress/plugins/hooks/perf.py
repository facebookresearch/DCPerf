#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import logging
import os

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)


DEFAULT_BACKGROUND_DURATION_SECS = "60"
DEFAULT_OPTIONS = ["-e", "cycles,instructions,cache-references,cache-misses,bus-cycles"]
DEFAULT_PATH = "perf.real stat"


class Perf(Hook):
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
            "{}_perf_hook_output_{}".format(job_name, iteration_num),
        )

        self.background_mode = False
        if "background_mode" in opts and opts["background_mode"]:
            self.background_mode = True
            self.bg_perf_proc = self._start_background_perf(opts)
        else:
            binary_path = job.binary
            job.args = opts["args"] + ["--", binary_path] + job.args
            job.binary = opts.get("binary_path", DEFAULT_PATH)

    def after_job(self, opts, job):
        if self.background_mode:
            if self.bg_perf_proc and self.bg_perf_proc.poll() is None:
                self.bg_perf_proc.terminate()
                self.stdout.close()

    def _start_background_perf(self, opts):
        bg_opts = opts["background_mode"]
        duration = bg_opts.get("duration", DEFAULT_BACKGROUND_DURATION_SECS)
        stdout_path = bg_opts.get("stdout_path", self.default_stdout_path)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201

        cmd = opts.get("binary_path", DEFAULT_PATH).split()
        cmd += opts["args"]
        cmd += ["-I", "1000", "-a", "sleep", str(duration)]
        logging.info(f"Starting background 'perf' hook: {' '.join(cmd)}")

        return util.issue_background_command(cmd, self.stdout, self.stdout)
