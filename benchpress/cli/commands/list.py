#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import click
from benchpress.lib.job_listing import create_job_listing

from .command import BenchpressCommand, TABLE_FORMAT


class ListCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("list", help="list all configured jobs")
        parser.set_defaults(command=self)
        parser.add_argument(
            "-s",
            "--by-scope",
            action="store_true",
            help="list benchmarks by scope, i.e. app, micro, kernel",
        )
        parser.add_argument(
            "-c",
            "--by-component",
            action="store_true",
            help="list benchmarks by components being tested",
        )

    def run(self, args, jobs):
        if args.by_scope:
            group_key = "scope"
        elif args.by_component:
            group_key = "component"
        else:
            group_key = None

        job_list = []
        for job in jobs.values():
            if not hasattr(job, "config"):
                continue
            item = {
                "name": job.name,
                "description": job.description,
                "tags": job.tags,
            }
            job_list.append(item)
        click.echo(create_job_listing(job_list, TABLE_FORMAT, group_key))
