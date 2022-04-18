# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import os
import signal
import time

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

DEFAULT_PATH = ["mpstat"]
DEFAULT_OPTIONS = ["-u", "1"]
MAX_TRIES = 5
PROCESS_TERMINATE_DELAY = 3  # seconds


class CpuMpstat(Hook):
    """CpuMpstat collects cpu utilization in the background for the
    duration of the job.

    A background mpstat command is ran to collected cpu utilization at
    the provided poll interval (seconds). The data is recored under
    the benchmark metrics directory for the job as provided by the mpstat command
    and options to configured it.


    Example hook:

    ```yaml
    hooks:
      - hook: cpu-mpstat
        options:
          args:
            - '-u'   # utilization
            - '1'    # second interval
    ```
    """

    def __init__(self):
        self.background_process = None
        self.stdout = None

    def before_job(self, opts, job):
        if not opts:
            opts = {"args": DEFAULT_OPTIONS}
        if "args" not in opts:
            opts["args"] = DEFAULT_OPTIONS

        metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        stdout_path = os.path.join(
            metrics_dir, f"{job_name}_{job.uuid}_{iteration_num}_cpu_mpstat.txt"
        )
        # File descriptor closed in after_job(..)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201

        cmd = DEFAULT_PATH
        cmd += opts["args"]

        logging.info(f"Starting background 'cpu_mpstat' hook: {' '.join(cmd)}")
        self.background_process = util.issue_background_command(
            cmd, self.stdout, self.stdout
        )

    def after_job(self, opts, job):
        # Stop process if it's still running.
        if self.background_process and self.background_process.poll() is None:
            # Send SIGINT first, to gracefully stop
            self.background_process.send_signal(signal.SIGINT)
            # Check if process has not finished
            exited_cleanly = False
            for _ in range(MAX_TRIES):
                time.sleep(PROCESS_TERMINATE_DELAY)
                # check if the process has exited
                if self.background_process.poll() is None:
                    exited_cleanly = True
                    break

            if not exited_cleanly:
                # Exahusted tries, force terimnate
                # Send SIGTERM
                self.background_process.terminate()

        # Close stdout file descriptor
        if self.stdout:
            self.stdout.close()
