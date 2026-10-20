#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""NVIDIA Vera (Olympus) core, system-level-cache, and memory metrics."""

try:
    from cea.chips.benchpress.perfutils.arm_perf.core import (
        get_duration_series,
        register_arch,
        skip_if_missing,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf.core import (  # pyrefly: ignore[missing-import]
        get_duration_series,
        register_arch,
        skip_if_missing,
    )


def _align(grouped_df, numerator_event, denominator_event):
    numerator = grouped_df.get_group(numerator_event).counter_value
    denominator = grouped_df.get_group(denominator_event).counter_value
    denominator.index = numerator.index
    return numerator, denominator


def _safe_divide(numerator, denominator):
    return numerator.div(denominator.where(denominator != 0))


def _matching_groups(grouped_df, pmu_prefix, event):
    suffix = f"/{event}/"
    return [
        group
        for name, group in grouped_df
        if isinstance(name, str)
        and name.startswith(pmu_prefix)
        and name.endswith(suffix)
    ]


def _sum_event(grouped_df, pmu_prefix, event):
    groups = _matching_groups(grouped_df, pmu_prefix, event)
    if not groups:
        raise KeyError(f"{pmu_prefix}/{event}")
    total = groups[0].set_index("timestamp").counter_value.sort_index()
    for group in groups[1:]:
        values = group.set_index("timestamp").counter_value.sort_index()
        total = total.add(values)
    return total


def _mean_event(grouped_df, pmu_prefix, event):
    groups = _matching_groups(grouped_df, pmu_prefix, event)
    if not groups:
        raise KeyError(f"{pmu_prefix}/{event}")
    return _sum_event(grouped_df, pmu_prefix, event) / len(groups)


def _elapsed_seconds(grouped_df, pmu_prefix, event):
    groups = _matching_groups(grouped_df, pmu_prefix, event)
    if not groups:
        raise KeyError(f"{pmu_prefix}/{event}")
    group = groups[0].set_index("timestamp").sort_index()
    runtime_seconds = group.counter_runtime.astype("float") * 10**-9
    mux_fraction = group.mux.astype("float") / 100.0
    return _safe_divide(runtime_seconds, mux_fraction)


def _output_series(series, grouped_df):
    core_timestamps = (
        grouped_df.get_group("instructions").timestamp.sort_values().to_numpy()
    )
    return series.reindex(core_timestamps).reset_index(drop=True)


@skip_if_missing
def vera_timestamp(grouped_df):
    return {
        "name": "Timestamp_Secs",
        "series": grouped_df.get_group("cpu_cycles").timestamp,
    }


@skip_if_missing
def vera_mips(grouped_df):
    instructions = grouped_df.get_group("instructions").counter_value
    duration = get_duration_series(grouped_df.get_group("instructions"))
    return {
        "name": "MIPS",
        "series": instructions.astype("float").div(duration),
        "prefix": 10**-6,
    }


@skip_if_missing
def vera_ipc(grouped_df):
    instructions, cycles = _align(grouped_df, "instructions", "cpu_cycles")
    return {"name": "IPC", "series": _safe_divide(instructions, cycles)}


@skip_if_missing
def vera_branch_miss_rate(grouped_df):
    misses, branches = _align(grouped_df, "r22", "r21")
    return {
        "name": "Branch Miss Rate %",
        "series": _safe_divide(misses, branches),
        "prefix": 100,
    }


@skip_if_missing
def vera_l1_icache_miss_rate(grouped_df):
    refills, accesses = _align(grouped_df, "r01", "r14")
    return {
        "name": "L1 iCache Miss Rate %",
        "series": _safe_divide(refills, accesses),
        "prefix": 100,
    }


@skip_if_missing
def vera_l1_dcache_mpki(grouped_df):
    refills, instructions = _align(grouped_df, "r03", "instructions")
    return {
        "name": "L1 dCache MPKI",
        "series": _safe_divide(refills, instructions / 1000.0),
    }


@skip_if_missing
def vera_l1_dcache_miss_rate(grouped_df):
    refills, accesses = _align(grouped_df, "r03", "r04")
    return {
        "name": "L1 dCache Miss Rate %",
        "series": _safe_divide(refills, accesses),
        "prefix": 100,
    }


@skip_if_missing
def vera_ucf_mem_read_bw(grouped_df):
    bytes_read = _sum_event(grouped_df, "nvidia_ucf_pmu", "mem_bytes_rd")
    duration = _elapsed_seconds(grouped_df, "nvidia_ucf_pmu", "mem_bytes_rd")
    return {
        "name": "UCF Memory Read Bandwidth (MBps)",
        "series": _output_series(_safe_divide(bytes_read, duration), grouped_df),
        "prefix": 10**-6,
    }


@skip_if_missing
def vera_ucf_mem_write_bw(grouped_df):
    bytes_written = _sum_event(grouped_df, "nvidia_ucf_pmu", "mem_bytes_wr")
    duration = _elapsed_seconds(grouped_df, "nvidia_ucf_pmu", "mem_bytes_wr")
    return {
        "name": "UCF Memory Write Bandwidth (MBps)",
        "series": _output_series(_safe_divide(bytes_written, duration), grouped_df),
        "prefix": 10**-6,
    }


@skip_if_missing
def vera_slc_read_hit_rate(grouped_df):
    accesses = _sum_event(grouped_df, "nvidia_ucf_pmu", "slc_access_rd")
    hits = _sum_event(grouped_df, "nvidia_ucf_pmu", "slc_hit_rd")
    return {
        "name": "UCF SLC Read Hit Rate %",
        "series": _output_series(_safe_divide(hits, accesses), grouped_df),
        "prefix": 100,
    }


@skip_if_missing
def vera_slc_read_refill_mpki(grouped_df):
    refills = _sum_event(grouped_df, "nvidia_ucf_pmu", "slc_refill_rd")
    instructions = (
        grouped_df.get_group("instructions").set_index("timestamp").counter_value
    )
    return {
        "name": "UCF SLC Read Refill MPKI",
        "series": _output_series(
            _safe_divide(refills, instructions / 1000.0), grouped_df
        ),
    }


@skip_if_missing
def vera_cmem_read_latency(grouped_df):
    cycles = _mean_event(grouped_df, "nvidia_cmem_latency_pmu", "cycles")
    requests = _sum_event(grouped_df, "nvidia_cmem_latency_pmu", "rd_req")
    cumulative_outstanding = _sum_event(
        grouped_df, "nvidia_cmem_latency_pmu", "rd_cum_outs"
    )
    duration = _elapsed_seconds(grouped_df, "nvidia_cmem_latency_pmu", "cycles")
    frequency_hz = _safe_divide(cycles, duration)
    latency_seconds = _safe_divide(
        _safe_divide(cumulative_outstanding, requests), frequency_hz
    )
    return {
        "name": "CMEM Read Latency (ns)",
        "series": _output_series(latency_seconds, grouped_df),
        "prefix": 10**9,
    }


def metrics(grouped_df):
    return [
        vera_timestamp(grouped_df),
        vera_mips(grouped_df),
        vera_ipc(grouped_df),
        vera_branch_miss_rate(grouped_df),
        vera_l1_icache_miss_rate(grouped_df),
        vera_l1_dcache_mpki(grouped_df),
        vera_l1_dcache_miss_rate(grouped_df),
        vera_ucf_mem_read_bw(grouped_df),
        vera_ucf_mem_write_bw(grouped_df),
        vera_slc_read_hit_rate(grouped_df),
        vera_slc_read_refill_mpki(grouped_df),
        vera_cmem_read_latency(grouped_df),
    ]


register_arch(
    "vera",
    metrics,
    vendor="arm",
    align="shortest",
    description="NVIDIA Vera (Olympus) core PMU + UCF SLC/memory + CMEM latency",
)
