#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import logging

import click
from benchpress.lib.job import get_target_jobs
from benchpress.lib.util import clean_benchmark, clean_tool

from .command import BenchpressCommand


logger = logging.getLogger(__name__)


class CleanCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("clean", help="remove benchmark dependencies")
        parser.set_defaults(command=self)
        parser.add_argument("jobs", nargs="*", default=[], help="jobs to clean")

    def run(self, args, jobs):
        jobs = get_target_jobs(jobs, args.jobs)

        for job in jobs.values():
            # Clean out benchmark dependencies regardless if it's installed or not
            if job.cleanup_script != "":
                clean_benchmark(job.cleanup_script, job.install_script)
                click.echo("Cleaning out dependencies of {} job".format(job.name))
            else:
                click.echo(
                    f"Job {job.name} doesn't have a cleanup script in the jobs.yml file"
                )
            # install associated tools
            for hook in job.hooks:
                if clean_tool(hook[0]) > 0:
                    click.echo(
                        "Cleaning out associated tool {} for {}".format(
                            hook[0], job.name
                        )
                    )
