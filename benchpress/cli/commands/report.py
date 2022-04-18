#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import logging

from benchpress.lib.history import History
from benchpress.lib.job import get_target_jobs
from benchpress.lib.reporter_factory import ReporterFactory

from .command import BenchpressCommand

logger = logging.getLogger(__name__)


class ReportCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("report", help="report latest job results")
        parser.set_defaults(command=self)
        parser.add_argument("jobs", nargs="*", default=[], help="jobs to run")
        parser.add_argument("reporter", choices=ReporterFactory.registered_names)

    def run(self, args, jobs):
        reporter = ReporterFactory.create(args.reporter)

        jobs = get_target_jobs(jobs, args.jobs).values()

        history = History(args.results)

        for job in jobs:
            logger.info('Reporting result for "%s"', job.name)

            results = history.load_historical_results(job)
            if len(results) == 0:
                logger.warn('No historical results for "%s", skipping', job.name)
                continue

            latest = results[0]
            metrics = latest.metrics

            reporter.report(job, metrics)

        reporter.close()
