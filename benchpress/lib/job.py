#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import errno
import logging
import subprocess
import sys
import typing
from subprocess import CalledProcessError

import click
from benchpress.lib.job_listing import formalize_tags
from benchpress.lib.util import get_safe_cmd

from .hook_factory import HookFactory
from .parser_factory import ParserFactory

logger = logging.getLogger(__name__)

# Number of lines to output for stderr and stdout summary
TRIM_OUTPUT_LINES = 50


class Job(object):
    """Holds the run configuration for an individual job.
    A Job starts it's default config based on the benchmark configuration that
    it references. The binary defined in the benchmark is run according to the
    configuration of this job.

    Attributes:
        name (str): short name to identify job
        description (str): longer description to state intent of job
        tolerances (dict): percentage tolerance around the mean of historical
                           results
        config (dict): raw configuration dictionary
    """

    def __init__(self, job_config, benchmark_config, toolchain_config) -> None:
        """Create a Job with the default benchmark_config and the specific job
        config
        Args:
            config (dict): job config
            benchmark_config (dict): benchmark (aka default) config
            toolchain_config (dict): compiler and linker config

        # start with the config being the benchmark config and then update it
        # with the job config so that a job can override any options in the
        # benchmark config
        # keep this config because lib/history.py uses it
        config = benchmark_config
        config.update(job_config)

        toolchain config contains paths to the compilers, as well as compiler
        and linker flags used to build the benchmarks. It is a dict of
        different toolchain options from which `benchpress install` can choose.
        """
        self.benchmark_name = job_config["benchmark"]
        self.config = job_config

        self.name = job_config["name"]
        self.description = job_config["description"]
        self.install_script = benchmark_config.get("install_script", "")
        self.cleanup_script = benchmark_config.get("cleanup_script", "")
        self.stdout = job_config.get("stdout", "")
        self.uuid = job_config.get("uuid", "")
        self.timestamp = job_config["timestamp"]
        self.iteration_num = job_config["iteration_num"]

        self.binary = benchmark_config["path"]
        self.parser = ParserFactory.create(benchmark_config["parser"])
        self.check_returncode = benchmark_config.get("check_returncode", True)
        self.timeout = job_config.get("timeout", None)
        # if tee_output is True, the stdout and stderr commands of the child
        # process will be copied onto the stdout and stderr of benchpress
        # if this option is a string, the output will be written to the file
        # named by this value
        self.tee_output = job_config.get("tee_output", False)

        self.tags = formalize_tags([benchmark_config, job_config])

        self.hooks = job_config.get("hooks", [])
        self.hooks = [
            (h["hook"], HookFactory.create(h["hook"]), h.get("options", None))
            for h in self.hooks
        ]
        # self.hooks is list of (hook_name, hook, options)

        self.tolerances = job_config.get("tolerances", {})

        # roles are client/server or none
        self.roles = benchmark_config.get("roles", [])
        self.args = self.arg_list(job_config.get("args", []))
        self.vars = self.arg_list(job_config.get("vars", []))
        self.role_args = job_config.get("roles", {})

        self.toolchains = toolchain_config

    @staticmethod
    def arg_list(args):
        """Convert argument definitions to a list suitable for subprocess."""
        if isinstance(args, list):
            return args

        l = []
        for key, val in args.items():
            l.append("--" + key)
            if val is not None:
                l.append(str(val))
        return l

    def substitude_vars(self, role, role_input):
        if len(self.role_args) > 0:
            var_list = self.role_args[role].get("vars", [])
        else:
            var_list = self.vars
        if role_input is None:
            role_input = []
        new_dict = {}
        for k in var_list:
            if "=" in k:
                kv = k.split("=", maxsplit=2)
                k = kv[0]
                new_dict[k] = kv[1]
            if k in role_input:
                new_dict[k] = role_input[k]
                del role_input[k]
            if k not in new_dict:
                logger.error(f"The role '{role}' needs user input parameter '{k}'")
                exit(1)
        for k in role_input:
            logger.warning(f"Unrecognized user input parameter '{k}' for role '{role}'")
        if len(self.role_args) > 0:
            self.args = self.role_args[role].get("args", [])
        formatted_args = []
        for arg in self.args:
            formatted_args.append(arg.format(**new_dict))
        self.args = self.arg_list(formatted_args)

    def check_role(self, role, role_input):
        """move complex if else for role check here"""
        if len(self.role_args) == 0 and role != "":
            logger.error("the job {} does not have roles".format(self.name))
            exit(1)
        elif len(self.role_args) == 0 and role == "":
            self.substitude_vars(role, role_input)
        elif len(self.role_args) > 0 and role == "":
            logger.error("you must select a role in {} job".format(self.name))
            exit(1)
        elif len(self.role_args) > 0 and role != "":
            if role not in self.role_args:
                logger.error("must type correct role, current roles are:")
                logger.error("{}".format(", ".join(self.role_args.keys())))
                exit(1)
            self.substitude_vars(role, role_input)

    def copy_output(self, stderr, stdout):
        # optionally copy stdout/err of the child process to our own
        if self.tee_output:
            # default to stdout if no filename given
            tee = sys.stdout
            # if a file was specified, write to that file instead
            if isinstance(self.tee_output, str):
                with open(self.tee_output, "w") as tee:
                    # do this so each line is prefixed with stdout
                    for line in stdout.splitlines():
                        tee.write(f"stdout: {line}\n")
                    for line in stderr.splitlines():
                        tee.write(f"stderr: {line}\n")
                    # close the output if it was a file
                    if tee != sys.stdout:
                        tee.close()

    def start_hooks(self):
        """Executes hooks before job starts."""
        # take care of preprocessing setup via hook
        logger.info('Running setup hooks for "{}"'.format(self.name))
        for _name, hook, opts in self.hooks:
            logger.info("Running %s %s", hook, opts)
            hook.before_job(opts, self)

    def dry_run(self, role=None, role_input=None):
        """Just return the command line that will be used to run the
        selected benchmark
        """
        self.check_role(role, role_input)
        return get_safe_cmd([self.binary] + self.args)

    def run(self, role=None, role_input=None):
        """Run the benchmark and return the metrics that are reported.
        check if user type role correctly
        """
        self.check_role(role, role_input)

        try:
            logger.info('Starting "{}"'.format(self.name))
            cmd = get_safe_cmd([self.binary] + self.args)
            click.echo("Job execution command: {}".format(cmd))
            process = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            stdout, stderr = process.communicate(timeout=self.timeout)

            if self.stdout:
                with open(self.stdout, "r") as metrics_file:
                    stdout = metrics_file.read()
            else:
                stdout = stdout.decode("utf-8", "ignore")
            stderr = stderr.decode("utf-8", "ignore")
            self._print_output_summary(stdout, stderr)
            logger.info(f"fd=stdout output={stdout}")
            logger.info(f"fd=stderr output={stderr}")
            returncode = process.returncode
            if self.check_returncode and returncode != 0:
                logger.error(
                    'Job "{}" returned non-zero exit status {}'.format(
                        " ".join(cmd), returncode
                    )
                )
                exit(1)
                # raise CalledProcessError(process.returncode, cmd, output)
            self.copy_output(stderr, stdout)
            logger.info('Parsing results for "{}"'.format(self.name))
            try:
                return self.parser.parse(
                    stdout.splitlines(), stderr.splitlines(), returncode
                )
            except Exception:
                logger.error(
                    "Failed to parse results, this might mean the" " benchmark failed"
                )
                logger.error("stdout:\n{}".format(stdout))
                logger.error("stderr:\n{}".format(stderr))
                raise
        except OSError as e:
            logger.error('"{}" failed ({})'.format(self.name, e))
            if e.errno == errno.ENOENT:
                logger.error("Binary not found, did you forget to install it?")
            raise  # make sure it passes the exception up the chain
        except CalledProcessError as e:
            logger.error(e.output)
            raise  # make sure it passes the exception up the chain

    def stop_hooks(self):
        """Stops hooks after job is finished."""
        logger.info('Running cleanup hooks for "{}"'.format(self.name))
        # run hooks in reverse this time so it operates like a stack
        for _name, hook, opts in reversed(self.hooks):
            hook.after_job(opts, self)

    @property
    def safe_name(self) -> str:
        return self.name.replace(" ", "_")

    def _print_output_summary(self, stdout, stderr):
        stdout = stdout.splitlines()
        stderr = stderr.splitlines()
        output = "stdout:\n"
        if len(stdout) > TRIM_OUTPUT_LINES:
            output += f"\n[...trimmed to last {TRIM_OUTPUT_LINES} lines...]\n"
        output += "\t{}".format("\n\t".join(stdout[-TRIM_OUTPUT_LINES:]))

        output += "\nstderr:\n"
        if len(stderr) > TRIM_OUTPUT_LINES:
            output += f"\n[...trimmed to last {TRIM_OUTPUT_LINES} lines...]\n"
        output += "\t{}".format("\n\t".join(stderr[-TRIM_OUTPUT_LINES:]))
        click.echo(output)


class JobSuiteBuilder:
    """JobSuite is a collection of jobs that will be run as a group."""

    def __init__(self) -> None:
        self.suites = {}

    def add_job(self, job) -> None:
        for tag_group in job.tags.values():
            for tag in tag_group:
                if tag not in self.suites:
                    self.suites[tag] = []
                self.suites[tag].append(job.name)

    def get_suites(self) -> typing.Dict[str, str]:
        return self.suites


def get_target_jobs(all_jobs, args_jobs):
    picked_jobs = args_jobs if len(args_jobs) > 0 else list(all_jobs.keys())
    job_objs = {}
    for name in picked_jobs:
        if name not in all_jobs:
            logger.error('No job "{}" found'.format(name))
            exit(1)
        if hasattr(all_jobs[name], "config"):
            job_objs[name] = all_jobs[name]
        else:
            for suite_job_name in all_jobs[name]:
                job_objs[suite_job_name] = all_jobs[suite_job_name]
    return job_objs
