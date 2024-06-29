#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import abc
import logging
import os
import signal
import subprocess
import sys
import threading


logger = logging.getLogger(__name__)
# Path to directory of benchpress_cli.py
BP_BASEPATH = os.path.dirname(os.path.abspath(sys.argv[0]))


class Monitor:
    def __init__(self, interval, name, job_uuid):
        """Initialize some common parameters and storage variables"""
        self.name = name
        self.interval = interval
        # Reserved for result processing
        self.res = []
        # Reserved for original output of the monitoring process
        self.output = ""
        self.job_uuid = job_uuid
        self.logpath = BP_BASEPATH + f"/benchmark_metrics_{job_uuid}/{name}.log"
        self.csvpath = BP_BASEPATH + f"/benchmark_metrics_{job_uuid}/{name}.csv"
        self.logfile = open(self.logpath, "w", buffering=1)  # noqa: P201

    def __del__(self):
        self.logfile.close()

    def process_output(self, line):
        """Define custom ways to process each line of output"""
        pass

    def output_catcher(self):
        """Catch output from the monitoring process line by line, and do the following:
        1) Append the line into self.output variable
        2) Write the line to the log file at /path/to/benchpress/benchmark_metrics_<uuid>/<name>.log
        3) Call process_output(line) to let subclasses customly process output lines
        """
        if not hasattr(self, "proc"):
            return
        if not isinstance(self.proc, subprocess.Popen):
            return
        if self.proc.stdout is None:
            return
        for line in iter(self.proc.stdout.readline, ""):
            if not line:
                continue
            self.output += line
            self.process_output(line)
            self.logfile.write(line)

    def stderr_catcher(self):
        """Catch stderr from the monitoring process and send error messages to log"""
        if not hasattr(self, "proc"):
            return
        if not isinstance(self.proc, subprocess.Popen):
            return
        if self.proc.stderr is None:
            return
        for line in iter(self.proc.stderr.readline, ""):
            if not line:
                continue
            logger.warning(line)

    @abc.abstractmethod
    def run(self):
        """
        Here the subclasses should implement how to start the monitoring process
        using subprocess.Popen. They should also set stdout and stderr to
        subprocess.PIPE in order to utilize Monitor's built-in stdout and stderr
        catcher. After doing Popen, the subclass's run method can super-call this
        run method to start the stdout and stderr catcher. The output and stderr
        catcher instances will be recorded as `oc` and `ec` members.
        """
        self.oc = threading.Thread(
            target=self.output_catcher, name=self.name + "-stdout", args=()
        )
        self.oc.start()
        self.ec = threading.Thread(
            target=self.stderr_catcher, name=self.name + "-stderr", args=()
        )
        self.ec.start()

    def terminate(self):
        """
        Kill the monitoring process using SIGTERM signal and join the stdout
        and stderr catcher threads.
        """
        exitcode = -1
        if hasattr(self, "proc") and isinstance(self.proc, subprocess.Popen):
            os.kill(self.proc.pid, signal.SIGTERM)
            exitcode = self.proc.wait()
        if hasattr(self, "oc") and isinstance(self.oc, threading.Thread):
            self.oc.join()
        if hasattr(self, "ec") and isinstance(self.ec, threading.Thread):
            self.ec.join()
        return exitcode

    def get_result(self):
        """Return the result array"""
        return self.res

    def gen_csv(self):
        if len(self.res) == 0:
            return ""

        csv_text = ""
        headers = sorted(self.res[0].keys() - {"timestamp"})
        csv_text += "index,timestamp," + ",".join(headers) + "\n"
        for i in range(len(self.res)):
            csv_text += f"{i},{self.res[i]['timestamp']},"
            for key in headers:
                csv_text += f"{self.res[i][key]},"
            csv_text += "\n"

        return csv_text

    def write_csv(self):
        csv = self.gen_csv()
        with open(self.csvpath, "w") as f:
            f.write(csv)
