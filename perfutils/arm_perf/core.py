#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""ARM-specific scaffolding for the ARM perf report generators.

Vendor-agnostic pieces (rendering, series aggregation, the ``--arch`` registry)
are imported from ``perf_report.core`` so they are defined once and shared with
the AMD package. This module adds only what is genuinely ARM-specific: the CSV
reader (with PMU-expansion dedup), single-socket handling, per-sample duration,
and the CMN uncore helpers.
"""

import pandas as pd

# Shared, vendor-agnostic surface (defined once in perf_report.core).
try:
    from cea.chips.benchpress.perfutils.perf_report.core import (  # noqa: F401
        aggregate_stats,
        ARCH_REGISTRY,
        arches_for_vendor,
        ArchSpec,
        concat_series,
        register_arch,
        render_as_csv,
        render_as_table,
        skip_if_missing,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from perf_report.core import (  # noqa: F401  # pyre-ignore[21]
        aggregate_stats,
        ARCH_REGISTRY,
        arches_for_vendor,
        ArchSpec,
        concat_series,
        register_arch,
        render_as_csv,
        render_as_table,
        skip_if_missing,
    )


def read_csv(perf_csv_file):
    df = pd.read_csv(
        perf_csv_file,
        names=[
            "timestamp",
            "counter_value",
            "counter_unit",
            "event_name",
            "counter_runtime",
            "mux",
            "optional_metric_value",
            "optional_metric_unit",
            "1",
            "2",
        ],
        dtype={
            "timestamp": "float64",
            "counter_value": "float64",
            "counter_unit": "str",
            "event_name": "str",
            "counter_runtime": "float64",
            "mux": "float",
        },
        na_values=["<not counted>"],
    )
    # Deduplicate: keep the last occurrence per (timestamp, event_name) in case
    # perf auto-expands certain events (e.g. system-wide cycles) into multiple
    # PMU instances per interval.
    df = df.drop_duplicates(subset=["timestamp", "event_name"], keep="last")
    return df


def get_num_sockets(group):
    return 1


def get_duration_series(group):
    ts_series = group.timestamp
    num_sockets = get_num_sockets(group)
    prev_ts_series = pd.Series(
        [0.0] * num_sockets + list(ts_series.iloc[:-num_sockets])
    )
    prev_ts_series.index = ts_series.index
    return ts_series.sub(prev_ts_series)


# ---------------------------------------------------------------------------
# Helper: align two grouped series by index
# ---------------------------------------------------------------------------
def _align(grouped_df, ev_a, ev_b):
    """Return (series_a, series_b) with matching indices."""
    sa = grouped_df.get_group(ev_a).counter_value
    sb = grouped_df.get_group(ev_b).counter_value
    sb.index = sa.index
    return sa, sb


def _sum_cmn_event(grouped_df, event_suffix):
    """Sum a CMN HN-S event across all mesh instances (arm_cmn_0, arm_cmn_1, ...).

    Arm CMN exposes one arm_cmn_N PMU per mesh instance (one per die on
    multi-die parts). This helper aggregates a given event across all
    discovered mesh instances so metrics reflect the full system-level cache.
    """
    total = None
    for name, group in grouped_df:
        if isinstance(name, str) and name.endswith(f"/{event_suffix}/"):
            if total is None:
                total = group.counter_value.copy()
            else:
                vals = group.counter_value
                vals.index = total.index
                total = total + vals
    if total is None:
        raise KeyError(event_suffix)
    return total.reset_index(drop=True)


def _sum_cspmu_config0(grouped_df):
    """Sum the DMC memory-controller PMU data-beat counter (config=0) across all
    active arm_cspmu_mc_<N> channels.

    Each arm_cspmu_mc_<N> instance is one DDR data (sub)channel; config=0 counts
    DDR data beats (32 B each). Summing the active channels gives the physical
    DRAM bandwidth (Arm "DMC Bandwidth Measurement", Phoenix SoC spec Table 15-6).
    """
    total = None
    for name, group in grouped_df:
        if (
            isinstance(name, str)
            and name.startswith("arm_cspmu_mc_")
            and "config=0" in name
        ):
            if total is None:
                total = group.counter_value.copy()
            else:
                vals = group.counter_value
                vals.index = total.index
                total = total + vals
    if total is None:
        raise KeyError("arm_cspmu_mc/config=0")
    return total.reset_index(drop=True)
