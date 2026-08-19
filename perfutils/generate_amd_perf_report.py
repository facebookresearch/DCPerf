#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""
Unified AMD performance report generator (thin CLI entry point).

Processes CSV output from the AMD ``collect_amd_*_perf_counters.sh`` scripts and
produces derived metrics (IPC, MPKI, miss rates, top-down breakdown, memory
bandwidth, etc.). One script serves every AMD Zen generation we benchmark; the
target microarchitecture is selected with ``--arch``.

The actual work lives in the ``amd_perf`` package:

  - ``amd_perf/core.py``          AMD-specific scaffolding (read_csv with
                                  socket columns, multi-socket handling,
                                  drop_first_interval, DRAM channel/freq
                                  discovery). Vendor-agnostic pieces (rendering,
                                  aggregation, the registry) come from
                                  perf_report.core, shared with the ARM package.
  - ``amd_perf/metrics.py``       all AMD derived-metric functions (shared
                                  across Zen generations).
  - ``amd_perf/arches/<cpu>.py``  one module per Zen generation. Each selects
                                  its metric list and calls ``register_arch``.

Adding a new AMD CPU is a two-step, additive change:

  1. Create ``amd_perf/arches/<cpu>.py`` selecting the CPU's metric list (add
     any new metric functions to ``metrics.py``) and calling
     ``register_arch("<cpu>", metrics, vendor="amd", ...)``.
  2. Add ``<cpu>`` to the import list in ``amd_perf/arches/__init__.py``.

No edits to this file, to core.py, or to any other CPU's module are required.
This mirrors the ARM consolidation in generate_arm_perf_report.py; both vendors
share one registry and rendering spine in perf_report.core.
"""

import itertools
import typing

import click

# Importing the arches package registers every AMD CPU via register_arch().
try:
    from cea.chips.benchpress.perfutils.amd_perf import arches  # noqa: F401
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from amd_perf import arches  # noqa: F401  # pyre-ignore[21]
try:
    from cea.chips.benchpress.perfutils.amd_perf.core import (
        ARCH_REGISTRY,
        arches_for_vendor,
        concat_series,
        drop_first_interval,
        get_num_sockets,
        read_csv,
        render_as_csv,
        render_as_table,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from amd_perf.core import (  # pyre-ignore[21]
        ARCH_REGISTRY,
        arches_for_vendor,
        concat_series,
        drop_first_interval,
        get_num_sockets,
        read_csv,
        render_as_csv,
        render_as_table,
    )

# AMD CPUs only. The registry is shared with the ARM package (perf_report.core),
# so scope the CLI to this vendor. sorted() keeps the original zen3/4/5/5es order.
_AMD_ARCHES = arches_for_vendor("amd")


@click.command()
@click.argument(
    "amd_perf_csv_file", type=click.Path(exists=True, dir_okay=False, resolve_path=True)
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
    type=click.Choice(_AMD_ARCHES),
    default="zen3",
    help="Specify which AMD architecture to aggregate counters correctly.",
)
def main(
    amd_perf_csv_file: click.Path,
    series: typing.TextIO,
    format: str,
    arch: str,
) -> None:
    spec = ARCH_REGISTRY[arch]
    df = read_csv(amd_perf_csv_file)
    grouped_df = df.groupby("event_name")
    metrics = spec.build(grouped_df)

    filtered_metrics = list(itertools.filterfalse(lambda x: x is None, metrics))

    if not filtered_metrics:
        click.echo(
            "No metrics could be calculated. All required counters are missing from the input data."
        )
        return

    filtered_metrics = drop_first_interval(filtered_metrics, get_num_sockets(df))

    # AMD aligns metrics to the longest series (declarative via spec.align).
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
