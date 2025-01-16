#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""A set of utilitites for consuming and validating config files.

Provides a set of exported constants to locate Benchpress' config
files from File-system agnostic way.

Typical usage example:

    import  yaml

    from benchpress import config

    with config.JOBS_CONFIG_PATH as jobs_path, jobs_path.open() as jobs_file:
        j = yaml.safe_load(jobs_file)


Constants:
    BENCHMARKS_CONFIG_PATH: Context manager for the benchmarks.yml path.
    JOBS_CONFIG_PATH: Context manager for the jobs.yml path.

"""

import importlib.resources
import logging
from contextlib import AbstractContextManager
from pathlib import Path
from typing import IO, Iterator, Union

import yaml
from benchpress.lib import open_source

# __package__ indicates relative to this package
BENCHMARKS_CONFIG_PATH: AbstractContextManager = importlib.resources.path(
    "benchpress.config", "benchmarks.yml"
)
JOBS_CONFIG_PATH: AbstractContextManager = importlib.resources.path(
    "benchpress.config", "jobs.yml"
)
ALT_BENCHMARKS_CONFIGS: dict[str, AbstractContextManager] = {}
ALT_JOBS_CONFIGS: dict[str, AbstractContextManager] = {}
TOOLCHAIN_CONFIG_PATH: AbstractContextManager = importlib.resources.path(
    "benchpress.config", "toolchain.yml"
)

logger = logging.getLogger(__name__)


def register_benchmark_suite(name):
    """
    Register an alternative benchmark suite. This essentially adds
    `benchmarks_<name>.yml` and `jobs_<name>.yml` under benchpress/config folder
    to the config sets.  Please make sure these two files exist when registering them.
    """
    ALT_BENCHMARKS_CONFIGS[name] = importlib.resources.path(
        "benchpress.config", f"benchmarks_{name}.yml"
    )
    ALT_JOBS_CONFIGS[name] = importlib.resources.path(
        "benchpress.config", f"jobs_{name}.yml"
    )


if not open_source:
    register_benchmark_suite("internal")
register_benchmark_suite("wdl")


class BenchpressConfig:
    def __init__(self):
        self.benchmarks_specs = None
        self.jobs_specs = None

    def load(
        self,
        benchmarks_specs_stream: Union[bytes, IO[bytes], str, IO[str]],
        jobs_specs_stream: Union[bytes, IO[bytes], str, IO[str]],
        toolchain_specs_stream: Union[bytes, IO[bytes], str, IO[str]],
    ):
        self.benchmarks_specs = yaml.safe_load(benchmarks_specs_stream)
        self.jobs_specs = yaml.safe_load(jobs_specs_stream)
        # pyre-fixme[16]: `BenchpressConfig` has no attribute `toolchain_specs`.
        self.toolchain_specs = yaml.safe_load(toolchain_specs_stream)

    def __repr__(self) -> str:
        # pyre-fixme[16]: `BenchpressConfig` has no attribute `benchmarks_specs_path`.
        # pyre-fixme[16]: `BenchpressConfig` has no attribute `jobs_specs_path`.
        return f"<BenchpressConfig benchmarks={self.benchmarks_specs_path} jobs={self.jobs_specs_path}"
