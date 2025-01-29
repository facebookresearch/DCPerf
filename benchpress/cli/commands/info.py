#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import pprint

import click
import tabulate
from benchpress.lib.job import get_target_jobs

from .command import BenchpressCommand, TABLE_FORMAT


class InfoCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser(
            "info", help="Provides more details about specific job."
        )
        parser.set_defaults(command=self)
        parser.add_argument(
            "job", help="Valid job name. Use 'list' command to see available jobs."
        )
        parser.add_argument("--json", action="store_true", help="print json format")

    def run(self, args, jobs):
        if args.job not in jobs:
            return

        job_list = get_target_jobs(jobs, [args.job]).values()

        if args.json:
            for job in job_list:
                json_str = json.dumps(job.config)
                click.echo(json_str)
        else:
            self._print_as_table(job_list)

    def _print_as_table(self, job_list):
        table = []
        for job in job_list:
            table.append(["--- Job ---", job.name])
            table.append(["Description", job.description])

            if "roles" in job.config:
                roles = ", ".join(job.config["roles"].keys())
                table.append(["Roles", pprint.pformat(roles)])
                table.append(["Arguments", pprint.pformat(job.config["roles"])])
            else:
                table.append(["Roles", ""])
                table.append(["Arguments", pprint.pformat(job.args)])

            table.append(["Hooks", pprint.pformat(job.config.get("hooks", ""))])

        click.echo(
            tabulate.tabulate(
                table, headers=["Properties", "Values"], tablefmt=TABLE_FORMAT
            )
        )
