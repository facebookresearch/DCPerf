#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import json
import os
import sys
from abc import ABCMeta, abstractmethod

from benchpress.lib import util


class Reporter(object, metaclass=ABCMeta):
    """A Reporter is used to record job results in your infrastructure."""

    @abstractmethod
    def report(self, job, metrics):
        """Save job metrics somewhere in existing monitoring infrastructure.

        Args:
            job (Job): job that was run
            metrics (dict): metrics that were exported by job
        """
        pass

    @abstractmethod
    def close(self):
        """Do whatever necessary cleanup is required after all jobs are finished."""
        pass


class StdoutReporter(Reporter):
    """Default reporter implementation, logs a JSON object to stdout."""

    def report(self, job, metrics):
        """Log JSON report to stdout.
        Attempt to detect whether a real person is running the program then
        pretty print the JSON, otherwise print it without linebreaks and
        unsorted keys.
        """
        # use isatty as a proxy for if a real human is running this
        if sys.stdout.isatty():
            json.dump(metrics, sys.stdout, sort_keys=True, indent=2)
        else:
            json.dump(metrics, sys.stdout)
        sys.stdout.write("\n")

    def close(self):
        pass


class JSONFileReporter(Reporter):
    """Reporter implementation to log job suite metrics to JSON file"""

    def report(self, job, metrics):
        """Log job suite metrics as dictionary to JSON file"""
        is_benchmark_metrics = "metrics" in metrics

        # Embed uuid in job suite dir and timestamp in metrics JSON file
        job_suite_run_id = metrics["run_id"]
        benchmark_metrics_dir = util.create_benchmark_metrics_dir(job_suite_run_id)
        job_suite_timestamp = metrics["timestamp"]
        job_suite_iteration_num = job.iteration_num

        job_name = job.name.replace(" ", "_")

        if is_benchmark_metrics:
            json_filename = "{}_metrics_{}_iter_{}.json".format(
                job_name, str(job_suite_timestamp), job_suite_iteration_num
            )
        else:
            json_filename = "{}_system_specs_{}.json".format(
                job_name, str(job_suite_timestamp)
            )
        json_filepath = os.path.join(benchmark_metrics_dir, json_filename)

        with open(json_filepath, "w+") as json_fp:
            json.dump(metrics, json_fp, sort_keys=True, indent=2)
            json_fp.write("\n")

    def close(self):
        pass
