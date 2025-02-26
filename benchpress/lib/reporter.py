#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import statistics as st
import sys
from abc import ABCMeta, abstractmethod

from benchpress.lib import baseline, util


class Reporter(metaclass=ABCMeta):
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


class ScoreReporter(Reporter):
    """Report scores of benchmarks as well as overall DCPerf score"""

    def report(self, job, metrics):
        if not hasattr(self, "scores"):
            self.scores = {}
        job_name = job.name.replace(" ", "_")

        if job_name not in baseline.JOB_TO_BM.keys():
            return

        if "score" in metrics:
            score = metrics["score"]
        else:
            score = baseline.get_score(job_name, metrics)
            if score is None:
                return

        bm_name = baseline.JOB_TO_BM[job_name]
        if bm_name not in self.scores:
            self.scores[bm_name] = []

        self.scores[bm_name].append(score)

    def sanitize_scores(self, score_list):
        """
        Remove zeros from score list
        Return sanitized score list
        """
        if not isinstance(score_list, list):
            return score_list
        return list(filter(lambda x: x > 0.0, score_list))

    def close(self):
        overall_scores = {}
        for bm, scores in self.scores.items():
            sanitized_scores = self.sanitize_scores(scores)
            if len(sanitized_scores) == 0:
                continue
            if len(sanitized_scores) == 1:
                overall_scores[bm] = scores[0]
                print(f"{bm}: {scores[0]:.3f}, single data point")
            elif len(sanitized_scores) == 2:
                avg_score = st.mean(sanitized_scores)
                overall_scores[bm] = avg_score
                print(f"{bm}: {avg_score:.3f}, avg of 2 data points")
            else:
                median = st.median(sanitized_scores)
                avg = st.mean(sanitized_scores)
                stdev = st.stdev(sanitized_scores)
                stdev_perc = stdev / avg * 100
                output = f"{bm}: {median:.3f}, median of {len(sanitized_scores)} data points, stdev {stdev_perc:.2f}%, mean {avg:.3f}"
                print(output)
                if median > 0:
                    overall_scores[bm] = median
        if len(overall_scores) > 0 and len(overall_scores) < 5:
            geomean_score = st.geometric_mean(overall_scores.values())
            print(f"DCPerf partial geomean: {geomean_score:.3f}")
        elif len(overall_scores) >= 5:
            geomean_score = st.geometric_mean(overall_scores.values())
            print(f"DCPerf overall score: {geomean_score:.3f}")
