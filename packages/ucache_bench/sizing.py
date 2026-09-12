#!/usr/bin/env fbpython
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import json
import math
import os
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Iterable


_SYSFS_ROOT: Path = Path("/sys")
_MEMINFO_PATH: Path = Path("/proc/meminfo")


class LoadVariant(str, Enum):
    PRODUCTION = "production"
    EXTREME = "extreme"


@dataclass(frozen=True)
class HardwareTopology:
    logical_cpus: int
    physical_cores: int
    memory_mib: int

    def __post_init__(self) -> None:
        if self.logical_cpus < 1:
            raise ValueError("logical_cpus must be positive")
        if self.physical_cores < 1:
            raise ValueError("physical_cores must be positive")
        if self.physical_cores > self.logical_cpus:
            raise ValueError("physical_cores cannot exceed logical_cpus")
        if self.memory_mib < 1024:
            raise ValueError("memory_mib must be at least 1024")

    @property
    def smt_enabled(self) -> bool:
        return self.logical_cpus > self.physical_cores

    @property
    def smt_coverage(self) -> float:
        return (self.logical_cpus - self.physical_cores) / self.physical_cores

    @property
    def uses_smt_capacity(self) -> bool:
        return self.smt_coverage >= 0.5

    @property
    def threads_per_core(self) -> float:
        return self.logical_cpus / self.physical_cores


@dataclass(frozen=True)
class ClientShape:
    aggregate_qps: int
    processes: int
    client_hosts: int
    num_proxies: int
    total_connections: int

    def __post_init__(self) -> None:
        if (
            min(
                self.aggregate_qps,
                self.processes,
                self.client_hosts,
                self.num_proxies,
                self.total_connections,
            )
            < 1
        ):
            raise ValueError("client shape values must be positive")
        if self.processes % self.client_hosts != 0:
            raise ValueError("processes must divide evenly across client hosts")
        max_connections = (
            self.processes * self.num_proxies * (32_768 // self.num_proxies)
        )
        if self.total_connections > max_connections:
            raise ValueError("connections exceed the per-process destination limit")

    @property
    def processes_per_host(self) -> int:
        return self.processes // self.client_hosts

    @property
    def open_loop_qps(self) -> int:
        return _round_to_nearest(self.aggregate_qps / self.processes, 250)

    @property
    def additional_fanout(self) -> int:
        connections_per_proxy = math.ceil(
            self.total_connections / (self.processes * self.num_proxies)
        )
        return max(0, connections_per_proxy - 1)


@dataclass(frozen=True)
class CalibrationAnchor:
    name: str
    topology: HardwareTopology
    cache_mb: int
    hash_power: int
    production: ClientShape
    extreme: ClientShape
    server_numa_interleave: bool = False

    def shape(self, variant: LoadVariant) -> ClientShape:
        if variant == LoadVariant.PRODUCTION:
            return self.production
        return self.extreme


@dataclass(frozen=True)
class Recommendation:
    topology: HardwareTopology
    variant: LoadVariant
    source: str
    calibration: str | None
    estimated_rru: float
    aggregate_qps: int
    total_client_processes: int
    total_connections: int
    params: dict[str, int | float | str]

    def to_dict(self) -> dict[str, object]:
        return {
            "variant": self.variant.value,
            "source": self.source,
            "calibration": self.calibration,
            "automark_benchmark": "ucache_bench_debug",
            "hardware": {
                "logical_cpus": self.topology.logical_cpus,
                "physical_cores": self.topology.physical_cores,
                "threads_per_core": round(self.topology.threads_per_core, 3),
                "smt_enabled": self.topology.smt_enabled,
                "smt_coverage": round(self.topology.smt_coverage, 3),
                "memory_mib": self.topology.memory_mib,
            },
            "derived": {
                "estimated_rru": round(self.estimated_rru, 2),
                "aggregate_offered_qps": self.aggregate_qps,
                "total_client_processes": self.total_client_processes,
                "total_connections": self.total_connections,
            },
            "automark_params": self.params,
        }


_CALIBRATIONS: tuple[CalibrationAnchor, ...] = (
    CalibrationAnchor(
        name="T2_TRN",
        topology=HardwareTopology(316, 158, 1_034_736),
        cache_mb=820_000,
        hash_power=31,
        production=ClientShape(5_000_000, 20, 1, 32, 100_480),
        extreme=ClientShape(4_800_000, 32, 4, 80, 401_920),
        server_numa_interleave=True,
    ),
    CalibrationAnchor(
        name="T1_BGM",
        topology=HardwareTopology(176, 88, 257_120),
        cache_mb=170_000,
        hash_power=29,
        production=ClientShape(2_500_000, 16, 1, 60, 133_440),
        extreme=ClientShape(2_800_000, 16, 2, 60, 133_440),
    ),
    CalibrationAnchor(
        name="T11_GRC_ARM",
        topology=HardwareTopology(72, 72, 260_682),
        cache_mb=160_000,
        hash_power=29,
        production=ClientShape(1_500_000, 16, 1, 20, 40_000),
        extreme=ClientShape(1_700_000, 16, 2, 20, 40_000),
    ),
    CalibrationAnchor(
        name="T1_MLN",
        topology=HardwareTopology(72, 36, 62_000),
        cache_mb=24_000,
        hash_power=28,
        production=ClientShape(700_000, 16, 1, 28, 51_520),
        extreme=ClientShape(1_000_000, 16, 2, 28, 51_520),
    ),
    CalibrationAnchor(
        name="T1_CPL",
        topology=HardwareTopology(52, 26, 62_000),
        cache_mb=24_000,
        hash_power=28,
        production=ClientShape(580_000, 16, 1, 20, 37_120),
        extreme=ClientShape(780_000, 16, 2, 20, 37_120),
    ),
)


def calibrations() -> tuple[CalibrationAnchor, ...]:
    return _CALIBRATIONS


def _round_to_nearest(value: float, quantum: int) -> int:
    return int(math.floor(value / quantum + 0.5)) * quantum


def _parse_cpu_list(value: str) -> set[int]:
    cpus: set[int] = set()
    for part in value.strip().split(","):
        if not part:
            continue
        if "-" in part:
            start, end = (int(item) for item in part.split("-", 1))
            cpus.update(range(start, end + 1))
        else:
            cpus.add(int(part))
    return cpus


def _physical_cores_from_sysfs(logical_cpus: Iterable[int], sysfs_root: Path) -> int:
    sibling_groups: set[tuple[int, ...]] = set()
    visible = set(logical_cpus)
    for cpu in visible:
        sibling_path = (
            sysfs_root
            / "devices"
            / "system"
            / "cpu"
            / f"cpu{cpu}"
            / "topology"
            / "thread_siblings_list"
        )
        try:
            siblings = _parse_cpu_list(sibling_path.read_text()) & visible
        except (OSError, ValueError):
            siblings = {cpu}
        sibling_groups.add(tuple(sorted(siblings or {cpu})))
    return len(sibling_groups)


def _memory_mib_from_meminfo(meminfo_path: Path) -> int:
    for line in meminfo_path.read_text().splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024
    raise ValueError(f"MemTotal is missing from {meminfo_path}")


def detect_hardware(
    *,
    sysfs_root: Path = _SYSFS_ROOT,
    meminfo_path: Path = _MEMINFO_PATH,
    affinity: Iterable[int] | None = None,
) -> HardwareTopology:
    visible_cpus = set(os.sched_getaffinity(0) if affinity is None else affinity)
    if not visible_cpus:
        raise ValueError("CPU affinity is empty")
    return HardwareTopology(
        logical_cpus=len(visible_cpus),
        physical_cores=_physical_cores_from_sysfs(visible_cpus, sysfs_root),
        memory_mib=_memory_mib_from_meminfo(meminfo_path),
    )


def estimate_rru(topology: HardwareTopology) -> float:
    memory_gib = topology.memory_mib / 1024
    return 2 / 3 + topology.physical_cores / 15 + memory_gib / 30


def calculate_cache_mb(topology: HardwareTopology) -> int:
    memory_mib = topology.memory_mib
    if memory_mib <= 48_000:
        cache_mib = memory_mib * 0.5
    elif memory_mib <= 256_000:
        cache_mib = 24_000 + (memory_mib - 48_000) * 146_000 / 208_000
    else:
        cache_mib = 170_000 + (memory_mib - 256_000) * 650_000 / 778_736
    return min(
        _round_to_nearest(cache_mib, 1_000),
        _round_to_nearest(memory_mib * 0.8, 1_000),
    )


def calculate_hash_power(cache_mb: int) -> int:
    if cache_mb <= 32_000:
        return 28
    if cache_mb <= 256_000:
        return 29
    return 31


def _fallback_num_proxies(topology: HardwareTopology) -> int:
    if not topology.uses_smt_capacity:
        return _round_to_nearest(max(20, topology.physical_cores / 4), 4)
    if topology.physical_cores >= 128:
        return 80
    if topology.physical_cores >= 64:
        return _round_to_nearest(topology.physical_cores * 0.68, 4)
    return _round_to_nearest(max(20, topology.physical_cores * 0.75), 4)


def _fallback_shape(topology: HardwareTopology, variant: LoadVariant) -> ClientShape:
    if variant == LoadVariant.PRODUCTION:
        client_hosts = 1
        processes = 20 if topology.physical_cores >= 128 else 16
        if topology.uses_smt_capacity and topology.physical_cores >= 128:
            qps_per_core = 31_500
        elif topology.uses_smt_capacity and topology.physical_cores >= 64:
            qps_per_core = 28_500
        else:
            qps_per_core = 21_000
    else:
        client_hosts = 4 if topology.physical_cores >= 128 else 2
        processes = client_hosts * 8
        if not topology.uses_smt_capacity:
            qps_per_core = 24_000
        elif topology.physical_cores >= 64:
            qps_per_core = 30_500
        else:
            qps_per_core = 29_000

    if topology.memory_mib >= 512 * 1024:
        connections_per_core = 2_550
    elif not topology.uses_smt_capacity:
        connections_per_core = 550
    elif topology.physical_cores >= 64:
        connections_per_core = 1_500
    else:
        connections_per_core = 1_400

    num_proxies = _fallback_num_proxies(topology)
    max_connections = processes * num_proxies * (32_768 // num_proxies)
    total_connections = min(
        _round_to_nearest(topology.physical_cores * connections_per_core, 1_000),
        max_connections,
    )
    return ClientShape(
        aggregate_qps=_round_to_nearest(topology.physical_cores * qps_per_core, 10_000),
        processes=processes,
        client_hosts=client_hosts,
        num_proxies=num_proxies,
        total_connections=total_connections,
    )


def find_calibration(topology: HardwareTopology) -> CalibrationAnchor | None:
    candidates: list[tuple[float, CalibrationAnchor]] = []
    for anchor in _CALIBRATIONS:
        logical_error = abs(topology.logical_cpus - anchor.topology.logical_cpus) / max(
            topology.logical_cpus, anchor.topology.logical_cpus
        )
        physical_error = abs(
            topology.physical_cores - anchor.topology.physical_cores
        ) / max(topology.physical_cores, anchor.topology.physical_cores)
        memory_error = abs(topology.memory_mib - anchor.topology.memory_mib) / max(
            topology.memory_mib, anchor.topology.memory_mib
        )
        has_memory_headroom = (
            anchor.cache_mb <= topology.memory_mib * 0.85
            and topology.memory_mib - anchor.cache_mb >= 16_000
        )
        if (
            logical_error <= 0.03
            and physical_error <= 0.03
            and memory_error <= 0.10
            and has_memory_headroom
        ):
            candidates.append(
                (logical_error + physical_error + 0.5 * memory_error, anchor)
            )
    if not candidates:
        return None
    return min(candidates, key=lambda item: item[0])[1]


def recommend(topology: HardwareTopology, variant: LoadVariant) -> Recommendation:
    anchor = find_calibration(topology)
    if anchor is None:
        cache_mb = calculate_cache_mb(topology)
        hash_power = calculate_hash_power(cache_mb)
        shape = _fallback_shape(topology, variant)
        source = "formula"
        calibration_name = None
        server_numa_interleave = topology.memory_mib >= 512 * 1024
    else:
        cache_mb = anchor.cache_mb
        hash_power = anchor.hash_power
        shape = anchor.shape(variant)
        source = "calibrated"
        calibration_name = anchor.name
        server_numa_interleave = anchor.server_numa_interleave

    params: dict[str, int | float | str] = {
        "memory_mb": cache_mb,
        "hash_power": hash_power,
        "key_count": cache_mb * 1000,
        "rpc_io_threads": topology.logical_cpus,
        "rpc_num_acceptor_threads": 4,
        "rpc_num_cpu_worker_threads": 1,
        "rpc_socket_max_reads_per_event": 16,
        "lru_rebalance_interval_sec": 1,
        "lru_rebalancing_hits_min_age_sec": 60,
        "lru_rebalancing_hits_max_age_sec": 7200,
        "hashtable_lock_power": 24,
        "cachelib_num_shards": 524288,
        "min_alloc_size": 64,
        "enable_fibers": 1,
        "production_features": 1,
        "zstd_compress_pct": 100,
        "zstd_level": 1,
        "duration_seconds": 240,
        "warmup_seconds": 720,
        "timeout_seconds": 2400,
        "use_distribution": 1,
        "distribution_config": "./packages/ucache_bench/traffic_dist.json",
        "failures_until_tko": 12,
        "num_client_hosts": shape.client_hosts,
        "num_source_ips": shape.processes_per_host - 1,
        "enable_random_source_ip": 1,
        "num_proxies": shape.num_proxies,
        "additional_fanout": shape.additional_fanout,
        "num_threads": 8,
        "max_inflight": 150,
        "open_loop_qps": shape.open_loop_qps,
        "open_loop_refill_on_miss": 0,
        "open_loop_max_outstanding": (
            1024 if variant == LoadVariant.PRODUCTION else 4096
        ),
        "open_loop_max_lateness_us": (
            1000 if variant == LoadVariant.PRODUCTION else 200000
        ),
        "connection_ramp_seconds": 25,
        "use_same_thread_client": 1,
        "server_numa_interleave": int(server_numa_interleave),
    }
    return Recommendation(
        topology=topology,
        variant=variant,
        source=source,
        calibration=calibration_name,
        estimated_rru=estimate_rru(topology),
        aggregate_qps=shape.aggregate_qps,
        total_client_processes=shape.processes,
        total_connections=shape.total_connections,
        params=params,
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Generate topology-aware UcacheBench parameters for the "
            "ucache_bench_debug Automark adapter"
        )
    )
    parser.add_argument(
        "--variant",
        choices=[variant.value for variant in LoadVariant],
        default=LoadVariant.PRODUCTION.value,
        help="production targets ~60%% CPU; extreme targets ~80%% CPU",
    )
    parser.add_argument("--logical-cpus", type=int)
    parser.add_argument("--physical-cores", type=int)
    parser.add_argument("--memory-mib", type=int)
    parser.add_argument(
        "--params-only",
        action="store_true",
        help="Print only the ucache_bench_debug params object",
    )
    parser.add_argument(
        "--list-calibrations",
        action="store_true",
        help="Print validated hardware anchors and exit",
    )
    return parser


def _resolve_topology(args: argparse.Namespace) -> HardwareTopology:
    if (
        args.logical_cpus is not None
        and args.physical_cores is not None
        and args.memory_mib is not None
    ):
        return HardwareTopology(
            logical_cpus=args.logical_cpus,
            physical_cores=args.physical_cores,
            memory_mib=args.memory_mib,
        )
    detected = detect_hardware()
    return HardwareTopology(
        logical_cpus=args.logical_cpus or detected.logical_cpus,
        physical_cores=args.physical_cores or detected.physical_cores,
        memory_mib=args.memory_mib or detected.memory_mib,
    )


def main() -> None:
    args = _parser().parse_args()
    if args.list_calibrations:
        output = [
            {
                "name": anchor.name,
                "hardware": {
                    "logical_cpus": anchor.topology.logical_cpus,
                    "physical_cores": anchor.topology.physical_cores,
                    "memory_mib": anchor.topology.memory_mib,
                },
            }
            for anchor in calibrations()
        ]
    else:
        recommendation = recommend(
            _resolve_topology(args), LoadVariant(args.variant)
        ).to_dict()
        output = (
            recommendation["automark_params"] if args.params_only else recommendation
        )
    print(json.dumps(output, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
