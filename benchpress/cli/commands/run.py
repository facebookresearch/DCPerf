#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import logging
import os
from datetime import datetime, timezone

import benchpress.lib.sys_specs as sys_specs
import click
from benchpress.lib.history import History
from benchpress.lib.hook_factory import HookFactory
from benchpress.lib.job import get_target_jobs
from benchpress.lib.reporter_factory import ReporterFactory
from benchpress.lib.util import verify_install

from .command import BenchpressCommand


logger = logging.getLogger(__name__)


class RunCommand(BenchpressCommand):
    def populate_parser(self, subparsers):
        parser = subparsers.add_parser("run", help="run job(s)")
        parser.set_defaults(command=self)
        parser.add_argument("jobs", nargs="*", default=[], help="jobs to run")
        parser.add_argument(
            "-r", "--role", default="", help="select roles for benchmark"
        )
        parser.add_argument(
            "-i",
            "--role_input",
            default={},
            help="role depended args, e.g. server_hostname",
        )
        parser.add_argument(
            "--disable-hooks",
            action="store_true",
            help="Disable execution of hooks for every job",
        )
        parser.add_argument(
            "--dry-run",
            action="store_true",
            help="Dry run to check commands to run",
        )
        parser.add_argument(
            "-k",
            "--hooks",
            default=[],
            nargs="+",
            help="Additional hooks to apply, e.g. --hooks hook1 hook2 hook3 ...",
        )
        parser.add_argument(
            "-a",
            "--hook-args",
            default="{}",
            help='Hook arguments in JSON format, e.g. \'{"hook1": {"key": <value>, ...}, "hook2": {...}, ...}\'',
        )

    def run(self, args, jobs) -> None:
        json_reporter = ReporterFactory.create("json_file")

        jobs = get_target_jobs(jobs, args.jobs).values()

        click.echo("Will run {} job(s)".format(len(jobs)))

        history = History(args.results)
        now = datetime.now(timezone.utc)

        cpu_topology = sys_specs.get_cpu_topology()
        os_kernel_data = sys_specs.get_os_kernel()
        os_release_data = sys_specs.get_os_release_data()
        kernel_cmdline = sys_specs.get_kernel_cmdline()
        dmidecode_data = sys_specs.get_dmidecode_data()
        sys_packages = []
        if "id" in os_release_data:
            os_id = os_release_data["id"].lower()
            if os_id in ("centos", "rhel", "fedora"):
                sys_packages = sys_specs.get_rpm_packages()
            elif os_id in ("ubuntu", "debian"):
                sys_packages = sys_specs.get_dpkg_packages()
            else:
                sys_packages = []
        kernel_params = sys_specs.get_sysctl_data()
        mem_data = sys_specs.get_cpu_mem_data()
        hw_data = sys_specs.get_hw_data()

        sys_specs_dict = {}
        sys_specs_dict["cpu_topology"] = cpu_topology
        sys_specs_dict["os_kernel"] = os_kernel_data
        sys_specs_dict["kernel_cmdline"] = kernel_cmdline
        sys_specs_dict["dmidecode"] = dmidecode_data
        sys_specs_dict["sys_packages"] = sys_packages
        sys_specs_dict["kernel_params"] = kernel_params
        sys_specs_dict["memory"] = mem_data
        sys_specs_dict["hardware"] = hw_data
        sys_specs_dict["os-release"] = os_release_data

        final_metrics = {}
        if "machines" not in final_metrics:
            final_metrics["machines"] = []
        machine_data = {}
        machine_data["hostname"] = os_kernel_data.get("node_name", "")
        machine_data["os_release_name"] = os_release_data.get("pretty_name", "")
        machine_data["os_distro"] = os_release_data.get("id", "")
        machine_data["kernel_version"] = os_kernel_data.get("kernel_release", "")
        machine_data["cpu_architecture"] = os_kernel_data.get("machine", "")
        machine_data["cpu_model"] = cpu_topology.get("Model name", "")
        machine_data["num_logical_cpus"] = cpu_topology.get("CPU(s)", "")
        machine_data["mem_total_kib"] = mem_data.get("MemTotal", "")
        final_metrics["machines"].append(machine_data)

        final_metrics["metadata"] = {}
        final_metrics["metadata"]["L1d cache"] = cpu_topology.get("L1d cache", "")
        final_metrics["metadata"]["L1i cache"] = cpu_topology.get("L1i cache", "")
        final_metrics["metadata"]["L2 cache"] = cpu_topology.get("L2 cache", "")
        final_metrics["metadata"]["L3 cache"] = cpu_topology.get("L3 cache", "")

        role_in = {}
        if args.role_input:
            try:
                role_in = json.loads(args.role_input)
            except Exception:
                click.echo("role_input must be json dictionary format")
                click.echo("example input format for iperf:")
                click.echo(
                    './benchpress run iperf --role client --role_input=\'{"server_hostname":"rtptest1234.prn1"}\''
                )
                exit(1)

        for job in jobs:
            if not verify_install(job.install_script):
                click.echo("Benchmark {} not installed".format(job.name))
                continue
            click.echo('Running "{}": {}'.format(job.name, job.description))

            if args.dry_run:
                job_cmd = job.dry_run(args.role, role_in)
                click.echo(f"Execution command: {' '.join(job_cmd)}")
                continue

            try:
                additional_hook_args = json.loads(args.hook_args)
            except json.JSONDecodeError:
                logger.warning(
                    "Could not parse hook args - please make sure it's in valid JSON format"
                )
                additional_hook_args = {}

            additional_hooks = args.hooks
            for hook in additional_hooks:
                hook_opts = None
                if hook in additional_hook_args:
                    hook_opts = additional_hook_args[hook]
                job.hooks.append((hook, HookFactory.create(hook), hook_opts))

            if args.disable_hooks:
                click.echo("Hooks globally disabled as requested")
            else:
                job.start_hooks()
            metrics_dir = f"benchmark_metrics_{job.uuid}"
            now = datetime.now()
            date = now.strftime("%Y%m%d_%H%M")
            symlink = job.name + "_timestamp:" + date + "_" + job.uuid
            os.symlink(metrics_dir, f"benchmark_metrics_{symlink}")
            sys_specs_dict["run_id"] = job.uuid
            sys_specs_dict["timestamp"] = job.timestamp

            final_metrics["run_id"] = job.uuid
            final_metrics["timestamp"] = job.timestamp

            final_metrics["benchmark_name"] = job.name
            final_metrics["benchmark_desc"] = job.description
            # Hooks structured as: hook_name: hook_options
            job_hooks = ["{}: {}".format(hook[0], hook[2]) for hook in job.hooks]
            final_metrics["benchmark_hooks"] = job_hooks

            try:
                metrics = job.run(args.role, role_in)
            except Exception:
                # Continue to propagate exception up the stack
                raise
            finally:
                # Fill benchmark_args after running to make sure
                # variables are substituted
                final_metrics["benchmark_args"] = job.args
                # Make sure hooks are stopped, even if job failed
                if not args.disable_hooks:
                    job.stop_hooks()

            final_metrics["metrics"] = metrics
            stdout_reporter = ReporterFactory.create("stdout")
            click.echo("Results Report:")
            stdout_reporter.report(job, final_metrics)

            json_reporter.report(job, final_metrics)
            json_reporter.report(job, sys_specs_dict)

            history.save_job_result(job, metrics, now)

            click.echo(
                'Finished running "{}": {} with uuid: {}'.format(
                    job.name, job.description, job.uuid
                )
            )

        json_reporter.close()
