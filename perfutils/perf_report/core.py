#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Vendor-agnostic scaffolding shared by every perf report generator.

Both the ARM and AMD report packages import from here so the rendering, series
aggregation, and the ``--arch`` registry are defined exactly once. Vendor- and
CPU-specific logic (CSV parsing, socket handling, per-CPU metric functions)
lives in the per-vendor packages (``arm_perf``, ``amd_perf``); this module is
the common spine that makes both vendors consistent and expandable.

Adding a new CPU (either vendor) is an additive change:
  1. Add a module under <vendor>_perf/arches/<cpu>.py defining its metric
     functions and calling ``register_arch(...)`` at import time.
  2. Add <cpu> to that vendor package's arches/__init__.py import list.
No edits to this file or to any other CPU's module are required.
"""

import csv
import dataclasses
import functools
import io
import typing

import pandas as pd
import tabulate


def skip_if_missing(f):
    @functools.wraps(f)
    def wrap(*args, **kwargs):
        try:
            return f(*args, **kwargs)
        except KeyError:
            pass

    return wrap


def aggregate_stats(derived_metric):
    derived_series = derived_metric["series"]
    prefix = derived_metric.get("prefix", 1.0)
    return {
        "min": derived_series.min() * prefix,
        "mean": derived_series.mean() * prefix,
        "std": derived_series.std() * prefix,
        "p95": derived_series.quantile(0.95) * prefix,
        "max": derived_series.max() * prefix,
    }


def render_as_csv(metrics, delimiter=","):
    output = io.StringIO()
    csv_writer = csv.writer(output, delimiter=delimiter)
    csv_writer.writerow(["metric", "mean", "stddev", "min", "p95", "max"])
    for metric in metrics:
        stats = aggregate_stats(metric)
        csv_writer.writerow(
            [
                metric["name"],
                stats["mean"],
                stats["std"],
                stats["min"],
                stats["p95"],
                stats["max"],
            ]
        )
    return output.getvalue()


def render_as_table(metrics):
    headers = ["Metric", "Mean", "StdDev", "Min", "P95", "Max"]
    table = []
    for metric in metrics:
        stats = aggregate_stats(metric)
        row = [
            metric["name"],
            round(stats["mean"], 4),
            round(stats["std"], 4),
            round(stats["min"], 4),
            round(stats["p95"], 4),
            round(stats["max"], 4),
        ]
        table.append(row)
    return tabulate.tabulate(
        table, headers, tablefmt="simple", stralign="left", numalign="right"
    )


def concat_series(metrics, shortest_length_series):
    short_series = shortest_length_series["series"]
    series = []
    for m in metrics:
        m["series"].index = short_series.index
        m["series"].name = m["name"]
        prefix = m.get("prefix", 1.0)
        series.append(m["series"] * prefix)
    return pd.concat(series, axis=1).reset_index()


# ===========================================================================
# Architecture registry
# ===========================================================================
#
# One registry, shared by every vendor. Each CPU registers an ArchSpec keyed by
# its --arch value. Per-CPU quirks (e.g. which series length to align to) are
# declarative data on the spec, not branches in any main().


@dataclasses.dataclass(frozen=True)
class ArchSpec:
    """Everything a vendor's main() needs to render one CPU's report.

    name:    ``--arch`` value, e.g. "grace" or "zen5".
    vendor:  "arm" or "amd" -- lets a shared surface group CPUs by vendor.
    build:   grouped_df -> list[metric | None]; the per-CPU metric list.
    align:   pivot series length when concatenating metrics: "shortest"
             (Grace / Neoverse V3) or "longest" (Axion / all AMD). Replaces the
             old per-arch ``pivot = max if ...`` branch in each main().
    description: short human-readable label for --help / docs.
    """

    name: str
    vendor: str
    build: typing.Callable[[typing.Any], list]
    align: str = "shortest"
    description: str = ""


ARCH_REGISTRY: "dict[str, ArchSpec]" = {}


def register_arch(
    name: str,
    build: typing.Callable[[typing.Any], list],
    *,
    vendor: str,
    align: str = "shortest",
    description: str = "",
) -> None:
    """Register one CPU. Called at import time by each arch module."""
    if align not in ("shortest", "longest"):
        raise ValueError(f"align must be 'shortest' or 'longest', got {align!r}")
    if name in ARCH_REGISTRY:
        raise ValueError(f"arch {name!r} already registered")
    ARCH_REGISTRY[name] = ArchSpec(
        name=name, vendor=vendor, build=build, align=align, description=description
    )


def arches_for_vendor(vendor: str) -> "list[str]":
    """Sorted --arch names registered for a given vendor."""
    return sorted(n for n, s in ARCH_REGISTRY.items() if s.vendor == vendor)
