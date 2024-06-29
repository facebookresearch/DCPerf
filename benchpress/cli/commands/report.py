#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import logging

from benchpress.lib.history import History
from benchpress.lib.job import get_target_jobs
from benchpress.lib.reporter_factory import ReporterFactory

from .command import BenchpressCommand

logger = logging.getLogger(__name__)


class ReportCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("report", help="report job results")
        parser.set_defaults(command=self)
        parser.add_argument("jobs", nargs="*", default=[], help="jobs to run")
        parser.add_argument("reporter", choices=ReporterFactory.registered_names)
        parser.add_argument(
            "--all",
            action="store_true",
            default=False,
            help="report all benchmark results instead of only the latest one",
        )

    def run(self, args, jobs):
        reporter = ReporterFactory.create(args.reporter)

        jobs = get_target_jobs(jobs, args.jobs).values()

        history = History(args.results)

        for job in jobs:
            logger.info('Reporting result for "%s"', job.name)

            results = history.load_historical_results(job)
            if len(results) == 0:
                logger.info('No historical results for "%s", skipping', job.name)
                continue

            if not args.all:
                latest = results[0]
                metrics = latest.metrics
                metrics["run_id"] = latest.run_id
                metrics["timestamp"] = latest.timestamp
                reporter.report(job, metrics)
            else:
                for res in results:
                    metrics = res.metrics
                    metrics["run_id"] = res.run_id
                    metrics["timestamp"] = res.timestamp
                    reporter.report(job, metrics)

        reporter.close()
