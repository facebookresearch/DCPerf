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
from typing import Iterable, Sequence


_SYSFS_ROOT: Path = Path("/sys")
_MEMINFO_PATH: Path = Path("/proc/meminfo")
_CGROUP_ROOT: Path = Path("/sys/fs/cgroup")
_PROC_SELF_CGROUP: Path = Path("/proc/self/cgroup")
_MIB_BYTES: int = 1024 * 1024
_CACHE_CAP_MIB: int = 1024 * 1024
_CACHE_BASE_RESERVE_MIB: int = 32 * 1024
_CACHE_USAGE_FACTOR: float = 0.75
_MEMORY_RICH_MIB: int = 512 * 1024
_DESTINATIONS_PER_PROCESS: int = 32_768
_BASE_TARGET_DESTINATIONS_PER_PROCESS: int = 11_000
_TARGET_DESTINATIONS_PER_CORE: int = 64
_MAX_TARGET_DESTINATIONS_PER_PROCESS: int = 13_250
_MAX_TARGET_TOTAL_CONNECTIONS: int = 220_000
_MAX_TOTAL_PROCESSES: int = 64
_MAX_PROXIES: int = 80


class LoadVariant(str, Enum):
    PRODUCTION = "production"
    EXTREME = "extreme"

    @property
    def target_cpu(self) -> float:
        return 0.60 if self == LoadVariant.PRODUCTION else 0.80

    @property
    def process_divisor(self) -> int:
        return 8 if self == LoadVariant.PRODUCTION else 6

    @property
    def process_quantum(self) -> int:
        return 4 if self == LoadVariant.PRODUCTION else 8

    @property
    def max_outstanding(self) -> int:
        return 8192

    @property
    def max_lateness_us(self) -> int:
        return 500_000


@dataclass(frozen=True)
class HardwareTopology:
    logical_cpus: int
    physical_cores: int
    memory_mib: int
    numa_memory_nodes: int = 1

    def __post_init__(self) -> None:
        _validate_cpu_topology(self.logical_cpus, self.physical_cores)
        if self.memory_mib < 4096:
            raise ValueError("usable memory must be at least 4096 MiB")
        if self.numa_memory_nodes < 1:
            raise ValueError("numa_memory_nodes must be positive")

    @property
    def effective_cores(self) -> float:
        return effective_cores(self.logical_cpus, self.physical_cores)

    @property
    def substantial_smt(self) -> bool:
        return has_substantial_smt(self.logical_cpus, self.physical_cores)


@dataclass(frozen=True)
class ServerResources:
    reserve_mib: int
    cache_mib: int
    key_count: int
    hash_power: int
    io_threads: int
    acceptor_threads: int
    lock_power: int
    cache_shards: int


@dataclass(frozen=True)
class ClientShape:
    client_hosts: int
    processes_per_host: int
    num_proxies: int
    total_connections: int

    def __post_init__(self) -> None:
        if min(self.client_hosts, self.processes_per_host, self.num_proxies) < 1:
            raise ValueError("client topology values must be positive")
        if not 1 <= self.num_proxies <= _MAX_PROXIES:
            raise ValueError(f"num_proxies must be in [1, {_MAX_PROXIES}]")
        if self.total_connections < self.connection_quantum:
            raise ValueError("total_connections cannot leave a proxy unused")
        if self.total_connections % self.connection_quantum != 0:
            raise ValueError("total_connections must divide across all proxies")
        if self.total_connections > self.max_total_connections:
            raise ValueError("connections exceed the per-process destination limit")

    @property
    def total_processes(self) -> int:
        return self.client_hosts * self.processes_per_host

    @property
    def connection_quantum(self) -> int:
        return self.total_processes * self.num_proxies

    @property
    def max_total_connections(self) -> int:
        destinations_per_proxy = _DESTINATIONS_PER_PROCESS // self.num_proxies
        return self.connection_quantum * destinations_per_proxy

    @property
    def additional_fanout(self) -> int:
        return self.total_connections // self.connection_quantum - 1


@dataclass(frozen=True)
class Recommendation:
    topology: HardwareTopology
    variant: LoadVariant
    resources: ServerResources
    client_shape: ClientShape
    aggregate_qps: int | None
    latency_seeded: bool
    params: dict[str, int | float | str]

    def to_dict(self) -> dict[str, object]:
        derived: dict[str, int | float] = {
            "target_cpu": self.variant.target_cpu,
            "reserve_mib": self.resources.reserve_mib,
            "cache_mib": self.resources.cache_mib,
            "key_count": self.resources.key_count,
            "server_io_threads": self.resources.io_threads,
            "total_client_processes": self.client_shape.total_processes,
            "total_connections": self.client_shape.total_connections,
        }
        if self.aggregate_qps is not None:
            per_process_qps = int(self.params["open_loop_qps"])
            derived["aggregate_qps"] = self.aggregate_qps
            derived["effective_aggregate_qps"] = (
                per_process_qps * self.client_shape.total_processes
            )

        return {
            "variant": self.variant.value,
            "hardware": {
                "logical_cpus": self.topology.logical_cpus,
                "physical_cores": self.topology.physical_cores,
                "effective_cores": self.topology.effective_cores,
                "substantial_smt": self.topology.substantial_smt,
                "usable_memory_mib": self.topology.memory_mib,
                "numa_memory_nodes": self.topology.numa_memory_nodes,
            },
            "derived": derived,
            "calibration": {
                "required": self.aggregate_qps is None or self.latency_seeded,
                "seeded_from_baseline_latency": self.latency_seeded,
                "target_cpu": self.variant.target_cpu,
            },
            "benchmark_params": self.params,
        }


def _validate_cpu_topology(logical_cpus: int, physical_cores: int) -> None:
    if logical_cpus < 1:
        raise ValueError("logical_cpus must be positive")
    if physical_cores < 1:
        raise ValueError("physical_cores must be positive")
    if physical_cores > logical_cpus:
        raise ValueError("physical_cores cannot exceed logical_cpus")


def _clamp(lower: int, upper: int, value: int) -> int:
    return min(upper, max(lower, value))


def _round_up(value: float, quantum: int) -> int:
    return math.ceil(value / quantum) * quantum


def _round_to_nearest_multiple(value: float, quantum: int) -> int:
    return math.floor(value / quantum + 0.5) * quantum


def _floor_to_multiple(value: float, quantum: int) -> int:
    return math.floor(value / quantum) * quantum


def effective_cores(logical_cpus: int, physical_cores: int) -> float:
    _validate_cpu_topology(logical_cpus, physical_cores)
    sibling_threads = min(physical_cores, logical_cpus - physical_cores)
    return physical_cores + 0.25 * sibling_threads


def has_substantial_smt(logical_cpus: int, physical_cores: int) -> bool:
    _validate_cpu_topology(logical_cpus, physical_cores)
    return logical_cpus - physical_cores >= 0.5 * physical_cores


def _parse_cpu_list(value: str) -> set[int]:
    cpus: set[int] = set()
    for part in value.strip().split(","):
        if not part:
            continue
        if "-" in part:
            start, end = (int(item) for item in part.split("-", 1))
            if end < start:
                raise ValueError(f"invalid CPU range: {part}")
            cpus.update(range(start, end + 1))
        else:
            cpus.add(int(part))
    return cpus


def _physical_cores_from_sysfs(logical_cpus: Iterable[int], sysfs_root: Path) -> int:
    sibling_groups: list[set[int]] = []
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
        except OSError:
            siblings = {cpu}
        merged = siblings or {cpu}
        overlapping = [group for group in sibling_groups if group & merged]
        for group in overlapping:
            merged.update(group)
            sibling_groups.remove(group)
        sibling_groups.append(merged)
    return len(sibling_groups)


def _numa_memory_node_count(sysfs_root: Path) -> int:
    node_root = sysfs_root / "devices" / "system" / "node"
    nodes = 0
    for node in node_root.glob("node[0-9]*"):
        try:
            for line in (node / "meminfo").read_text().splitlines():
                fields = line.split()
                if (
                    len(fields) >= 4
                    and fields[0] == "Node"
                    and fields[2] == "MemTotal:"
                    and int(fields[3]) > 0
                ):
                    nodes += 1
                    break
        except (OSError, ValueError):
            continue
    return max(1, nodes)


def _memory_mib_from_meminfo(meminfo_path: Path) -> int:
    for line in meminfo_path.read_text().splitlines():
        if line.startswith("MemTotal:"):
            return int(line.split()[1]) // 1024
    raise ValueError(f"MemTotal is missing from {meminfo_path}")


def _current_cgroup_directory(cgroup_root: Path, cgroup_file: Path) -> Path:
    try:
        lines = cgroup_file.read_text().splitlines()
    except OSError:
        return cgroup_root

    for line in lines:
        if not line.startswith("0::"):
            continue
        relative = Path(line[3:].lstrip("/"))
        if ".." in relative.parts:
            return cgroup_root
        candidate = cgroup_root / relative
        return candidate if candidate.is_dir() else cgroup_root
    return cgroup_root


def _read_cgroup_limit_mib(path: Path) -> int | None:
    try:
        value = path.read_text().strip()
    except OSError:
        return None
    if value == "max":
        return None
    try:
        return int(value) // _MIB_BYTES
    except ValueError as error:
        raise ValueError(f"invalid cgroup memory limit in {path}: {value}") from error


def _cgroup_memory_limits_mib(cgroup_root: Path, cgroup_file: Path) -> list[int]:
    current = _current_cgroup_directory(cgroup_root, cgroup_file)
    limits: list[int] = []
    while current == cgroup_root or cgroup_root in current.parents:
        for name in ("memory.max", "memory.high"):
            limit = _read_cgroup_limit_mib(current / name)
            if limit is not None:
                limits.append(limit)
        if current == cgroup_root:
            break
        current = current.parent
    return limits


def _detect_hardware_values(
    *,
    sysfs_root: Path = _SYSFS_ROOT,
    meminfo_path: Path = _MEMINFO_PATH,
    cgroup_root: Path = _CGROUP_ROOT,
    cgroup_file: Path = _PROC_SELF_CGROUP,
    affinity: Iterable[int] | None = None,
) -> tuple[int, int, int, int]:
    visible_cpus = set(os.sched_getaffinity(0) if affinity is None else affinity)
    if not visible_cpus:
        raise ValueError("CPU affinity is empty")

    memory_limits = [
        _memory_mib_from_meminfo(meminfo_path),
        *_cgroup_memory_limits_mib(cgroup_root, cgroup_file),
    ]
    return (
        len(visible_cpus),
        _physical_cores_from_sysfs(visible_cpus, sysfs_root),
        min(memory_limits),
        _numa_memory_node_count(sysfs_root),
    )


def detect_hardware(
    *,
    sysfs_root: Path = _SYSFS_ROOT,
    meminfo_path: Path = _MEMINFO_PATH,
    cgroup_root: Path = _CGROUP_ROOT,
    cgroup_file: Path = _PROC_SELF_CGROUP,
    affinity: Iterable[int] | None = None,
) -> HardwareTopology:
    logical_cpus, physical_cores, memory_mib, numa_memory_nodes = (
        _detect_hardware_values(
            sysfs_root=sysfs_root,
            meminfo_path=meminfo_path,
            cgroup_root=cgroup_root,
            cgroup_file=cgroup_file,
            affinity=affinity,
        )
    )
    return HardwareTopology(
        logical_cpus=logical_cpus,
        physical_cores=physical_cores,
        memory_mib=memory_mib,
        numa_memory_nodes=numa_memory_nodes,
    )


def calculate_reserve_mib(usable_memory_mib: int) -> int:
    return min(_CACHE_BASE_RESERVE_MIB, usable_memory_mib // 2)


def calculate_cache_mib(usable_memory_mib: int) -> int:
    if usable_memory_mib < 1:
        raise ValueError("usable_memory_mib must be positive")
    reserve_mib = calculate_reserve_mib(usable_memory_mib)
    capacity_mib = min(
        _CACHE_USAGE_FACTOR * (usable_memory_mib - reserve_mib),
        _CACHE_CAP_MIB,
    )
    cache_mib = _floor_to_multiple(capacity_mib, 64)
    if cache_mib < 64:
        raise ValueError("usable memory leaves less than 64 MiB for the cache")
    return cache_mib


def calculate_hash_power(cache_mib: int) -> int:
    if cache_mib < 1:
        raise ValueError("cache_mib must be positive")
    if cache_mib <= 64 * 1024:
        return 28
    if cache_mib <= 256 * 1024:
        return 29
    if cache_mib <= 512 * 1024:
        return 30
    return 31


def calculate_resources(
    topology: HardwareTopology, average_item_bytes: int = 1024
) -> ServerResources:
    if average_item_bytes < 1:
        raise ValueError("average_item_bytes must be positive")

    reserve_mib = calculate_reserve_mib(topology.memory_mib)
    cache_mib = calculate_cache_mib(topology.memory_mib)
    return ServerResources(
        reserve_mib=reserve_mib,
        cache_mib=cache_mib,
        key_count=cache_mib * _MIB_BYTES // average_item_bytes,
        hash_power=calculate_hash_power(cache_mib),
        io_threads=topology.logical_cpus,
        acceptor_threads=4,
        lock_power=24,
        cache_shards=524_288,
    )


def calculate_total_processes(physical_cores: int, variant: LoadVariant) -> int:
    if physical_cores < 1:
        raise ValueError("physical_cores must be positive")
    if variant == LoadVariant.PRODUCTION:
        requested = _round_up(physical_cores / 8, 4)
        return min(_MAX_TOTAL_PROCESSES, max(16, requested))
    requested = max(16, physical_cores / variant.process_divisor)
    return min(
        _MAX_TOTAL_PROCESSES,
        _round_up(requested, variant.process_quantum),
    )


def calculate_proxy_count(topology: HardwareTopology, variant: LoadVariant) -> int:
    if not topology.substantial_smt:
        requested = max(20, topology.physical_cores / 4)
    elif topology.memory_mib >= _MEMORY_RICH_MIB:
        divisor = 5 if variant == LoadVariant.PRODUCTION else 2
        requested = topology.physical_cores / divisor
    elif topology.physical_cores >= 64:
        requested = 0.68 * topology.physical_cores
    else:
        requested = max(20, 0.75 * topology.physical_cores)
    rounded = _round_to_nearest_multiple(requested, 4)
    return _clamp(4, _MAX_PROXIES, rounded)


def _calculate_client_topology(
    topology: HardwareTopology,
    variant: LoadVariant,
) -> tuple[int, int, int]:
    total_processes = calculate_total_processes(topology.physical_cores, variant)
    if variant == LoadVariant.PRODUCTION:
        client_hosts = 1
        processes_per_host = total_processes
    else:
        processes_per_host = 8
        client_hosts = math.ceil(total_processes / processes_per_host)
    return client_hosts, processes_per_host, calculate_proxy_count(topology, variant)


def calculate_target_connections(topology: HardwareTopology) -> int:
    total_processes = max(
        calculate_total_processes(topology.physical_cores, variant)
        for variant in LoadVariant
    )
    destinations_per_process = min(
        _MAX_TARGET_DESTINATIONS_PER_PROCESS,
        _BASE_TARGET_DESTINATIONS_PER_PROCESS
        + _TARGET_DESTINATIONS_PER_CORE * topology.physical_cores,
    )
    return min(
        _MAX_TARGET_TOTAL_CONNECTIONS,
        total_processes * destinations_per_process,
    )


def calculate_total_connections(
    topology: HardwareTopology, variant: LoadVariant
) -> int:
    client_hosts, processes_per_host, num_proxies = _calculate_client_topology(
        topology, variant
    )
    total_processes = client_hosts * processes_per_host
    quantum = total_processes * num_proxies
    max_connections = quantum * (_DESTINATIONS_PER_PROCESS // num_proxies)
    target_connections = calculate_target_connections(topology)
    aligned_connections = _round_up(target_connections, quantum)
    if aligned_connections > max_connections:
        raise ValueError(
            f"client topology cannot reach {target_connections} total "
            f"connections without exceeding {_DESTINATIONS_PER_PROCESS} "
            "destinations per process"
        )
    return aligned_connections


def calculate_client_shape(
    topology: HardwareTopology,
    variant: LoadVariant,
) -> ClientShape:
    client_hosts, processes_per_host, num_proxies = _calculate_client_topology(
        topology, variant
    )
    return ClientShape(
        client_hosts=client_hosts,
        processes_per_host=processes_per_host,
        num_proxies=num_proxies,
        total_connections=calculate_total_connections(topology, variant),
    )


def seed_qps(server_io_threads: int, baseline_latency_us: float) -> int:
    if server_io_threads < 1:
        raise ValueError("server_io_threads must be positive")
    if not math.isfinite(baseline_latency_us) or baseline_latency_us <= 0:
        raise ValueError("baseline_latency_us must be positive and finite")
    return max(
        1,
        math.floor(0.10 * server_io_threads * 1_000_000 / baseline_latency_us),
    )


def next_qps(current_qps: int, observed_cpu: float, target_cpu: float) -> int:
    if current_qps < 1:
        raise ValueError("current_qps must be positive")
    if not math.isfinite(observed_cpu) or observed_cpu <= 0:
        raise ValueError("observed_cpu must be positive and finite")
    if not math.isfinite(target_cpu) or not 0 < target_cpu <= 1:
        raise ValueError("target_cpu must be in (0, 1]")
    step = min(1.50, max(0.67, target_cpu / observed_cpu))
    return max(1, round(current_qps * step))


def recommend(
    topology: HardwareTopology,
    variant: LoadVariant,
    *,
    average_item_bytes: int = 1024,
    aggregate_qps: int | None = None,
    baseline_latency_us: float | None = None,
) -> Recommendation:
    if aggregate_qps is not None and aggregate_qps < 1:
        raise ValueError("aggregate_qps must be positive")
    if baseline_latency_us is not None and (
        not math.isfinite(baseline_latency_us) or baseline_latency_us <= 0
    ):
        raise ValueError("baseline_latency_us must be positive and finite")

    resources = calculate_resources(topology, average_item_bytes)
    shape = calculate_client_shape(topology, variant)

    latency_seeded = aggregate_qps is None and baseline_latency_us is not None
    resolved_qps = aggregate_qps
    if latency_seeded:
        resolved_qps = seed_qps(resources.io_threads, baseline_latency_us)
    if resolved_qps is not None and resolved_qps < shape.total_processes:
        raise ValueError(
            "aggregate_qps must be at least the derived client process count "
            f"({shape.total_processes})"
        )

    params: dict[str, int | float | str] = {
        "memory_mb": resources.cache_mib,
        "hash_power": resources.hash_power,
        "key_count": resources.key_count,
        "rpc_io_threads": resources.io_threads,
        "rpc_num_acceptor_threads": resources.acceptor_threads,
        "rpc_num_cpu_worker_threads": 1,
        "server_numa_interleave": int(topology.numa_memory_nodes > 1),
        "hashtable_lock_power": resources.lock_power,
        "cachelib_num_shards": resources.cache_shards,
        "min_alloc_size": 64,
        "enable_fibers": 1,
        "duration_seconds": 240,
        "warmup_seconds": 720,
        "timeout_seconds": 2400,
        "use_distribution": 1,
        "distribution_config": "./packages/ucache_bench/traffic_dist.json",
        "num_client_hosts": shape.client_hosts,
        "num_source_ips": shape.processes_per_host - 1,
        "enable_random_source_ip": 1,
        "num_proxies": shape.num_proxies,
        "additional_fanout": shape.additional_fanout,
        "num_threads": 8,
        "max_inflight": 150,
        "warmup_max_inflight": 32,
        "open_loop_refill_on_miss": 0,
        "open_loop_max_outstanding": variant.max_outstanding,
        "open_loop_max_lateness_us": variant.max_lateness_us,
        "connection_ramp_seconds": 60,
        "failures_until_tko": 12,
        "use_same_thread_client": 1,
    }
    if resolved_qps is not None:
        params["open_loop_qps"] = max(1, resolved_qps // shape.total_processes)
        params["process_ramp_seconds"] = 64
        params["full_load_stabilization_seconds"] = 60

    return Recommendation(
        topology=topology,
        variant=variant,
        resources=resources,
        client_shape=shape,
        aggregate_qps=resolved_qps,
        latency_seeded=latency_seeded,
        params=params,
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate topology-aware UcacheBench parameters"
    )
    parser.add_argument(
        "--variant",
        choices=[variant.value for variant in LoadVariant],
        default=LoadVariant.PRODUCTION.value,
        help="production targets 60%% CPU; extreme targets 80%% CPU",
    )
    parser.add_argument("--logical-cpus", type=int)
    parser.add_argument("--physical-cores", type=int)
    parser.add_argument(
        "--memory-mib", help="usable memory after container limits", type=int
    )
    parser.add_argument(
        "--numa-memory-nodes",
        type=int,
        help="NUMA nodes with memory (auto-detected unless topology is overridden)",
    )
    parser.add_argument("--average-item-bytes", default=1024, type=int)
    parser.add_argument("--aggregate-qps", type=int)
    parser.add_argument("--baseline-latency-us", type=float)
    parser.add_argument(
        "--params-only",
        action="store_true",
        help="print only the benchmark parameter object",
    )
    return parser


def _resolve_topology(args: argparse.Namespace) -> HardwareTopology:
    supplied = (args.logical_cpus, args.physical_cores, args.memory_mib)
    if all(value is not None for value in supplied):
        return HardwareTopology(
            logical_cpus=args.logical_cpus,
            physical_cores=args.physical_cores,
            memory_mib=args.memory_mib,
            numa_memory_nodes=(
                args.numa_memory_nodes if args.numa_memory_nodes is not None else 1
            ),
        )
    (
        detected_logical,
        detected_physical,
        detected_memory,
        detected_numa_memory_nodes,
    ) = _detect_hardware_values()
    return HardwareTopology(
        logical_cpus=(
            args.logical_cpus if args.logical_cpus is not None else detected_logical
        ),
        physical_cores=(
            args.physical_cores
            if args.physical_cores is not None
            else detected_physical
        ),
        memory_mib=(
            args.memory_mib if args.memory_mib is not None else detected_memory
        ),
        numa_memory_nodes=(
            args.numa_memory_nodes
            if args.numa_memory_nodes is not None
            else detected_numa_memory_nodes
        ),
    )


def main(argv: Sequence[str] | None = None) -> None:
    parser = _parser()
    args = parser.parse_args(argv)
    try:
        topology = _resolve_topology(args)
        recommendation = recommend(
            topology,
            LoadVariant(args.variant),
            average_item_bytes=args.average_item_bytes,
            aggregate_qps=args.aggregate_qps,
            baseline_latency_us=args.baseline_latency_us,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    if args.params_only and recommendation.aggregate_qps is None:
        parser.error(
            "--params-only requires --aggregate-qps or --baseline-latency-us; "
            "calibrate offered load before generating runnable parameters"
        )
    output = recommendation.params if args.params_only else recommendation.to_dict()
    print(json.dumps(output, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
