#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""
Unified ARM performance report generator (thin CLI entry point).

Processes CSV output from the ARM ``collect_*_perf_counters.sh`` scripts and
produces derived metrics (IPC, MPKI, miss rates, top-down breakdown, memory
bandwidth, etc.). One script serves every ARM CPU we benchmark; the target
microarchitecture is selected with ``--arch``.

The actual work lives in the ``arm_perf`` package:

  - ``arm_perf/core.py``          shared scaffolding (read_csv, renderers,
                                  series alignment, PMU helpers) + the arch
                                  registry.
  - ``arm_perf/arches/<cpu>.py``  one module per CPU. Each defines its metric
                                  functions and calls ``register_arch(...)``.

Adding a new ARM CPU is a two-step, additive change:

  1. Create ``arm_perf/arches/<cpu>.py`` with the CPU's metric functions and a
     ``register_arch("<cpu>", metrics, align=..., description=...)`` call.
  2. Add ``<cpu>`` to the import list in ``arm_perf/arches/__init__.py``.

No edits to this file, to core.py, or to any other CPU's module are required --
that is the whole point of the registry: the report generator scales to more
CPUs without a growing monolith or a hand-maintained if/else in ``main()``.

This mirrors (and improves on) the AMD consolidation in
generate_amd_perf_report.py.
"""

import itertools
import typing

import click

# Importing the arches package registers every ARM CPU via register_arch().
try:
    from cea.chips.benchpress.perfutils.arm_perf import arches  # noqa: F401
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf import arches  # noqa: F401  # pyre-ignore[21]
try:
    from cea.chips.benchpress.perfutils.arm_perf.core import (
        ARCH_REGISTRY,
        arches_for_vendor,
        concat_series,
        read_csv,
        render_as_csv,
        render_as_table,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf.core import (  # pyre-ignore[21]
        ARCH_REGISTRY,
        arches_for_vendor,
        concat_series,
        read_csv,
        render_as_csv,
        render_as_table,
    )

# ARM CPUs only. The registry is shared with the AMD package (perf_report.core),
# so scope the CLI + alias to this vendor.
_ARM_ARCHES = arches_for_vendor("arm")

# Backward-compatible alias: {arch_name: builder}. Preserves the pre-registry
# public surface (tests and any external callers import ARCH_METRICS from here).
ARCH_METRICS = {name: ARCH_REGISTRY[name].build for name in _ARM_ARCHES}


@click.command()
@click.argument(
    "perf_csv_file", type=click.Path(exists=True, dir_okay=False, resolve_path=True)
)
@click.option(
    "-s",
    "--series",
    type=click.File(mode="w", lazy=True),
    help="Write derived time-series data as CSV into the designated file",
)
@click.option(
    "-f",
    "--format",
    type=click.Choice(["table", "csv"]),
    default="table",
    help="Output format",
)
@click.option(
    "-a",
    "--arch",
    type=click.Choice(_ARM_ARCHES),
    default="grace",
    help="Which ARM CPU's counter set to aggregate.",
)
def main(
    perf_csv_file: click.Path,
    series: typing.TextIO,
    format: str,
    arch: str,
) -> None:
    spec = ARCH_REGISTRY[arch]
    df = read_csv(perf_csv_file)
    grouped_df = df.groupby("event_name")
    metrics = spec.build(grouped_df)

    filtered_metrics = list(itertools.filterfalse(lambda x: x is None, metrics))
    # Per-CPU declarative alignment: Grace/Neoverse V3 align to the shortest
    # series, Axion to the longest. Preserves each generator's original
    # behavior without any arch-specific branch here in main().
    pivot = max if spec.align == "longest" else min
    pivot_series = pivot(filtered_metrics, key=lambda m: m["series"].size)
    df_metrics = concat_series(filtered_metrics, pivot_series)
    if series:
        series.write(df_metrics.to_csv(index=False))
    if format == "table":
        output = render_as_table(filtered_metrics)
    else:  # format == "csv"
        output = render_as_csv(filtered_metrics)
    click.echo(output)


if __name__ == "__main__":
    main()
