#!/usr/bin/env python3

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
from pathlib import Path
from typing import IO, Iterator, Union

import yaml

# __package__ indicates relative to this package
BENCHMARKS_CONFIG_PATH: Iterator[Path] = importlib.resources.path(
    "benchpress.config", "benchmarks.yml"
)
JOBS_CONFIG_PATH: Iterator[Path] = importlib.resources.path(
    "benchpress.config", "jobs.yml"
)
TOOLCHAIN_CONFIG_PATH: Iterator[Path] = importlib.resources.path(
    "benchpress.config", "toolchain.yml"
)

logger = logging.getLogger(__name__)


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
        self.toolchain_specs = yaml.safe_load(toolchain_specs_stream)

    def __repr__(self) -> str:
        return f"<BenchpressConfig benchmarks={self.benchmarks_specs_path} jobs={self.jobs_specs_path}"
