#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""AMD-specific scaffolding for the AMD perf report generators.

Vendor-agnostic pieces (rendering, series aggregation, the ``--arch`` registry)
are imported from ``perf_report.core`` so they are defined once and shared with
the ARM package. This module adds only what is genuinely AMD-specific: the CSV
reader (with socket/numcpus columns), multi-socket handling, per-sample
duration, first-interval dropping, and DRAM channel/frequency discovery.
"""

import subprocess

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


def read_csv(amd_perf_csv_file):
    df = pd.read_csv(
        amd_perf_csv_file,
        names=[
            "timestamp",
            "socket",
            "numcpus",
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
            "socket": "str",
            "numcpus": "int",
            "counter_value": "float64",
            "counter_unit": "str",
            "event_name": "str",
            "counter_runtime": "float64",
            "mux": "float",
        },
        na_values=["<not counted>"],
    )
    return df


def get_num_sockets(group):
    socket_series = group.socket
    return len(socket_series.reset_index().groupby("socket").index)


def get_duration_series(group):
    ts_series = group.timestamp
    num_sockets = get_num_sockets(group)
    prev_ts_series = pd.Series(
        [0.0] * num_sockets + list(ts_series.iloc[:-num_sockets])
    )
    prev_ts_series.index = ts_series.index
    return ts_series.sub(prev_ts_series)


def drop_first_interval(metrics, num_samples):
    """Drop the first perf-stat interval from every metric's time series.

    The uncore/DF memory-controller counters report a cumulative startup value
    on their first ``perf stat -I`` read, and the first interval's duration is
    often irregular, so the first sample's derived rates (e.g. DRAM bandwidth)
    can exceed the hardware's physical limits. Excluding it keeps both the
    aggregated summary and the emitted time series physically meaningful. One
    interval spans ``num_samples`` rows (one per socket). Series shorter than
    that are left untouched so short runs don't collapse to empty.
    """
    if any(m["series"].size <= num_samples for m in metrics):
        return metrics
    for m in metrics:
        m["series"] = m["series"].iloc[num_samples:]
    return metrics


def get_memory_info():
    """
    Returns the number of memory channels and the DDR frequency.
    Returns:
        tuple: A tuple containing the number of memory channels (int) and the DDR frequency (str).
    """
    # Run dmidecode command to get memory device information
    dmidecode_out = subprocess.check_output(
        ["sudo", "dmidecode", "--type", "17"]
    ).decode("utf-8")
    # Count only POPULATED memory channels on socket 0. `dmidecode --type 17` prints a
    # "Memory Device" block for every DIMM slot, including empty ones
    # ("Size: No Module Installed"). Counting every "Bank Locator" over-counted channels
    # on partially-populated systems (e.g. 10-of-12 on Turin), which inflated DRAM
    # bandwidth by (slots / populated) since DRAM BW scales linearly with num_channels.
    num_channels = 0
    for block in dmidecode_out.split("Memory Device")[1:]:
        block_lines = block.split("\n")
        bank_locator = next((ln for ln in block_lines if "Bank Locator" in ln), "")
        size = next((ln for ln in block_lines if ln.strip().startswith("Size:")), "")
        is_socket0 = any(x in bank_locator.lower() for x in ["p0", "socket 0", "node0"])
        is_populated = bool(size.strip()) and "no module installed" not in size.lower()
        if is_socket0 and is_populated:
            num_channels += 1
    # Get the DDR frequency
    ddr_freq_out = subprocess.check_output(["sudo", "dmidecode"]).decode("utf-8")
    ddr_freq_lines = ddr_freq_out.split("\n")
    for line in ddr_freq_lines:
        if "Configured Memory Speed" in line:
            ddr_freq = int(line.split(":")[1].strip().split()[0])
            break
    return num_channels, ddr_freq
