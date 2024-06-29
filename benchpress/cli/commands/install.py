#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import logging
import os

import click
from benchpress.lib.job import get_target_jobs
from benchpress.lib.util import (
    initialize_env_vars,
    install_benchmark,
    install_tool,
    verify_install,
)

from .command import BenchpressCommand

logger = logging.getLogger(__name__)


class InstallCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("install", help="install benchmarks")
        parser.set_defaults(command=self)
        parser.add_argument("jobs", nargs="*", default=[], help="jobs to install")
        parser.add_argument(
            "-f",
            "--force",
            action="store_true",
            help="Install again even if the benchmark is already installed",
        )
        parser.add_argument(
            "-t",
            "--toolchain",
            choices=["gcc", "clang"],
            default="gcc",
            help="Choose a compiler toolchain, and use its paths and flags defined "
            "in the toolchain config",
            type=str,
        )

    def run(self, args, jobs):
        jobs = get_target_jobs(jobs, args.jobs)

        for job in jobs.values():
            if not job.install_script:
                msg = (
                    "{} does not have install script," " try running without install it"
                )
                click.echo(msg.format(job.name))
                continue
            # install required tools
            for hook in job.hooks:
                retcode = install_tool(hook[0])
                if retcode > 0:
                    click.echo(
                        "Installing required tool {} for {}".format(hook[0], job.name)
                    )
                elif retcode == 0:
                    click.echo("Tool {} already installed".format(hook[0]))
            if args.force or not verify_install(job.install_script):
                click.echo(
                    "Installing benchmark for {}: {}".format(job.name, job.description)
                )
                # TODO(cltorres) Filter inherited environment vars
                env = os.environ
                env = initialize_env_vars(job, env=env, toolchain=args.toolchain)
                # Print env variables
                click.echo("******** env ********")
                for var in env:
                    click.echo(f"{var}={env[var]}")
                install_benchmark(job.install_script, env=env)
            else:
                click.echo("Benchmark for {} already installed".format(job.name))
