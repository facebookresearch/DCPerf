#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
import argparse
import io
import logging
import os
import shlex
import sys
import typing

import click
from benchpress import config, logging_config, PROJECT, VERSION
from benchpress.lib.job import Job, JobSuiteBuilder
from benchpress.lib.job_listing import create_job_listing
from benchpress.lib.reporter import JSONFileReporter, ScoreReporter, StdoutReporter
from benchpress.lib.reporter_factory import ReporterFactory
from benchpress.lib.util import generate_run_id, generate_timestamp
from benchpress.plugins.hooks import user_script

from .commands.clean import CleanCommand
from .commands.command import TABLE_FORMAT
from .commands.info import InfoCommand
from .commands.install import InstallCommand
from .commands.list import ListCommand
from .commands.report import ReportCommand
from .commands.run import RunCommand


logging_config.create_logger()
logger = logging.getLogger(__name__)


class Benchpress:
    def __init__(
        self,
        config: config.BenchpressConfig,
        uuid: typing.Optional[str] = None,
        timestamp: typing.Optional[int] = None,
        iteration_num: int = 1,
        override_job_args: typing.Optional[str] = None,
        hook_bg_duration: typing.Optional[str] = None,
        hook_path: typing.Optional[str] = None,
    ):
        self.config = config
        self.uuid = uuid
        self.timestamp = timestamp
        self.iteration_num = iteration_num

        # Generate an uuid, timestamp and iteration number if not given
        if uuid is None:
            self.uuid = generate_run_id()

        if timestamp is None:
            self.timestamp = generate_timestamp()

        self.hook_bg_duration = hook_bg_duration
        self.hook_path = hook_path
        self.override_job_args = override_job_args

        self._initialize()
        self._create_job_suites()
        logger.info(
            "Loaded {} benchmarks and {} jobs".format(
                len(self.config.benchmarks_specs), len(self.jobs)
            )
        )

    def _initialize(self):
        for j in self.config.jobs_specs:
            # All loaded jobs in one benchmark execution will have same uuid
            j["uuid"] = self.uuid
            j["timestamp"] = int(self.timestamp)
            j["iteration_num"] = self.iteration_num
            j["hook_bg_duration"] = self.hook_bg_duration
            if self.hook_path is not None:
                custom_hook = {
                    "hook": "user-script",
                    "options": {
                        "hook_path": self.hook_path,
                        "background_mode": {
                            "duration": user_script.DEFAULT_BACKGROUND_DURATION_SECS
                        },
                    },
                }
                if self.hook_bg_duration is not None:
                    custom_hook["options"]["background_mode"][
                        "duration"
                    ] = self.hook_bg_duration
                if "hooks" in j:
                    j["hooks"].append(custom_hook)
                else:
                    j["hooks"] = [custom_hook]

        jobs = [
            Job(
                j,
                self.config.benchmarks_specs[j["benchmark"]],
                self.config.toolchain_specs,
            )
            for j in self.config.jobs_specs
            if "tests" not in j
        ]
        self.jobs = {j.name: j for j in jobs}

    def _create_job_suites(self):
        # After all the regulars jobs are created, create the job suites
        builder = JobSuiteBuilder()
        for job in self.jobs.values():
            builder.add_job(job)

        for suite_name, job_names in builder.get_suites().items():
            if suite_name in self.jobs:
                logger.error(f"Name collision between Job and Tag: {suite_name}")
                exit(1)
            self.jobs[suite_name] = job_names

        if self.override_job_args:
            overridden_job, overridden_args = parse_override_job_args(
                self.override_job_args
            )
            for job in self.jobs:
                if overridden_job == job:
                    job_obj = self.jobs[job]
                    job_obj.args = overridden_args
                    self.jobs[job] = job_obj

    def list_jobs(self, group_key=None):
        job_list = []
        for job in self.jobs.values():
            if not hasattr(job, "config"):
                continue
            item = {
                "name": job.name,
                "description": job.description,
                "tags": job.tags,
            }
            job_list.append(item)
        return create_job_listing(job_list, TABLE_FORMAT, group_key)


def setup_parser():
    """Setup the commands and command line parser.

    Returns:
        setup parser (argparse.ArgumentParser)
    """
    commands = [
        ListCommand(),
        ReportCommand(),
        RunCommand(),
        InstallCommand(),
        CleanCommand(),
        InfoCommand(),
    ]

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-b",
        "--benchmarks",
        type=str,
        default=None,
        help="Optional override path to benchmarks file",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=str,
        dest="jobs_file",
        default=None,
        help="Optional override path to job configs file",
    )
    parser.add_argument(
        "--toolchain-config",
        dest="toolchain_file",
        help="Optional override path to toolchain config file",
        type=str,
    )
    parser.add_argument("-u", "--uuid", help="Unique run id of benchmark execution")
    parser.add_argument("-t", "--timestamp", help="Timestamp of benchmark execution")
    parser.add_argument(
        "-s",
        "--hook_path",
        help="Path of hook to execute in background of benchmark execution",
    )
    parser.add_argument(
        "-d",
        "--hook_bg_duration",
        help="Duration (seconds) of hook running in background of benchmark execution",
    )
    parser.add_argument(
        "-i", "--iteration_num", help="Iteration number of benchmark execution"
    )
    parser.add_argument(
        "-o",
        "--override_job_args",
        help="Override job args completely and directly from CLI",
        type=str,
    )

    subparsers = parser.add_subparsers(dest="command", help="subcommand to run")
    for command in commands:
        command.populate_parser(subparsers)

    subparsers.required = True

    parser.add_argument(
        "-r",
        "--results",
        metavar="results dir",
        default="./results",
        help="directory to load/store results",
    )
    parser.add_argument("--verbose", "-v", action="count", default=0)
    parser.add_argument("--version", action="version", version=f"{PROJECT} {VERSION}")

    return parser


def parse_override_job_args(override_job_args):
    try:
        job, args = override_job_args.split(":")
        job = job.strip()
        args = args.strip()
        args = shlex.split(args)
        return (job, args)
    except Exception:
        click.echo(
            (
                "Could not properly parse --override_job_args flag. "
                "Run ./automark exec -h to see an example of what to pass in."
            )
        )
        raise


def load_config(args) -> config.BenchpressConfig:
    """Load BenchpressConfig from jobs and benchmarks conifg files.

    If either `--jobs` or `--benchmarks` paths have been provided,
    override default configs to use those instead.
    """
    override_benchmark = False

    try:
        with config.BENCHMARKS_CONFIG_PATH as bench_path:
            benchmarks_specs = bench_path.open()
            if args.benchmarks:
                benchmarks_specs_path = os.path.abspath(args.benchmarks)

                # benchmarks file name overriding logic
                if (not os.path.exists(benchmarks_specs_path)) and (
                    "/" not in args.benchmarks
                ):
                    override_benchmark = True
                    logger.info(
                        'benchmarks file with name "{}" not found, overriding it'.format(
                            args.benchmarks
                        )
                    )
                    benchmarks_specs_path = os.path.abspath(
                        "./benchpress/config/benchmarks_" + args.benchmarks + ".yml"
                    )

                # benchmarks file path existence check
                if not os.path.exists(benchmarks_specs_path):
                    logger.error(
                        'benchmarks file with name "{}" not found'.format(
                            benchmarks_specs_path
                        )
                    )
                    exit(1)

                logger.warning("Overriding default benchmarks!")
                logger.info(
                    'Loading benchmarks from "{}"'.format(benchmarks_specs_path)
                )
                with open(benchmarks_specs_path) as bs:
                    # Reads everything into memory
                    benchmarks_specs = bs.read()

        with config.JOBS_CONFIG_PATH as jobs_path:
            jobs_specs = jobs_path.open()
            if override_benchmark:
                args.jobs_file = args.benchmarks

            if args.jobs_file:
                jobs_specs_path = os.path.abspath(args.jobs_file)

                # jobs file name overriding logic
                if not os.path.exists(jobs_specs_path) or override_benchmark:
                    jobs_specs_path = os.path.abspath(
                        "./benchpress/config/jobs_" + args.benchmarks + ".yml"
                    )

                # jobs file path existence check
                if not os.path.exists(jobs_specs_path):
                    logger.error(
                        'jobs file with name "{}" not found'.format(jobs_specs_path)
                    )
                    exit(1)

                logger.warning("Overriding default jobs!")
                logger.info('Loading jobs from "{}"'.format(jobs_specs_path))
                with open(jobs_specs_path) as jb:
                    # Reads everything into memory
                    jobs_specs = jb.read()

        with config.TOOLCHAIN_CONFIG_PATH as toolchain_path:
            toolchain_specs = toolchain_path.open()
            if args.toolchain_file:
                toolchain_specs_path = os.path.abspath(args.toolchain_file)
                logger.warning("Overriding default toolchain config!")
                logger.info(
                    'Loading toolchain config from "{}"'.format(toolchain_specs_path)
                )
                with open(toolchain_specs_path) as ts:
                    toolchain_specs = ts.read()

        conf = config.BenchpressConfig()
        conf.load(benchmarks_specs, jobs_specs, toolchain_specs)
    finally:
        # Close opened files if any
        for spec in [benchmarks_specs, jobs_specs, toolchain_specs]:
            if isinstance(spec, io.IOBase):
                spec.close()

    return conf


# ignore sys.argv[0] because that is the name of the program
def main(args=sys.argv[1:]):
    # register reporter plugins before setting up the parser
    ReporterFactory.register("stdout", StdoutReporter)
    ReporterFactory.register("json_file", JSONFileReporter)
    ReporterFactory.register("score", ScoreReporter)

    parser = setup_parser()
    args = parser.parse_args(args)

    conf = load_config(args)

    bp = Benchpress(
        conf,
        args.uuid,
        args.timestamp,
        args.iteration_num,
        args.override_job_args,
        args.hook_bg_duration,
        args.hook_path,
    )
    args.command.run(args, bp.jobs)
