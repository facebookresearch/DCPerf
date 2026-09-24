# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path
from unittest.mock import patch

from cea.chips.benchpress.packages.ucache_bench.sizing import (
    _parse_cpu_list,
    _physical_cores_from_sysfs,
    _resolve_topology,
    _round_to_nearest_multiple,
    calculate_cache_mib,
    calculate_client_shape,
    calculate_hash_power,
    calculate_proxy_count,
    calculate_reserve_mib,
    calculate_resources,
    calculate_target_connections,
    calculate_total_connections,
    calculate_total_processes,
    detect_hardware,
    HardwareTopology,
    has_substantial_smt,
    LoadVariant,
    main,
    next_qps,
    recommend,
    seed_qps,
)


class SizingTest(unittest.TestCase):
    def test_parse_cpu_list(self) -> None:
        self.assertEqual(_parse_cpu_list("0-3,8,10-11"), {0, 1, 2, 3, 8, 10, 11})

    def test_physical_core_detection_merges_partial_sibling_data(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sysfs_root = Path(tmp)
            for cpu, siblings in ((1, "1"), (2, "0,2")):
                topology_dir = (
                    sysfs_root / "devices" / "system" / "cpu" / f"cpu{cpu}" / "topology"
                )
                topology_dir.mkdir(parents=True)
                (topology_dir / "thread_siblings_list").write_text(siblings)

            physical_cores = _physical_cores_from_sysfs({0, 1, 2}, sysfs_root)

        self.assertEqual(physical_cores, 2)

    def test_detect_hardware_applies_affinity_and_cgroup_limits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for cpu, siblings in ((0, "0,2"), (1, "1,3"), (2, "0,2"), (3, "1,3")):
                topology_dir = (
                    root
                    / "sys"
                    / "devices"
                    / "system"
                    / "cpu"
                    / f"cpu{cpu}"
                    / "topology"
                )
                topology_dir.mkdir(parents=True)
                (topology_dir / "thread_siblings_list").write_text(siblings)

            for node in (0, 1):
                node_dir = root / "sys" / "devices" / "system" / "node" / f"node{node}"
                node_dir.mkdir(parents=True)
                (node_dir / "meminfo").write_text(
                    f"Node {node} MemTotal:       67108864 kB\n"
                )

            meminfo = root / "meminfo"
            meminfo.write_text("MemTotal:       134217728 kB\n")
            cgroup_root = root / "cgroup"
            cgroup = cgroup_root / "workload"
            cgroup.mkdir(parents=True)
            (cgroup_root / "memory.max").write_text(str(96 * 1024**3))
            (cgroup_root / "memory.high").write_text("max")
            (cgroup / "memory.max").write_text("max")
            (cgroup / "memory.high").write_text(str(64 * 1024**3))
            cgroup_file = root / "self.cgroup"
            cgroup_file.write_text("0::/workload\n")

            topology = detect_hardware(
                sysfs_root=root / "sys",
                meminfo_path=meminfo,
                cgroup_root=cgroup_root,
                cgroup_file=cgroup_file,
                affinity={0, 2, 3},
            )

        self.assertEqual(topology.logical_cpus, 3)
        self.assertEqual(topology.physical_cores, 2)
        self.assertEqual(topology.memory_mib, 64 * 1024)
        self.assertEqual(topology.numa_memory_nodes, 2)

    def test_partial_override_is_applied_before_validation(self) -> None:
        args = Namespace(
            logical_cpus=None,
            physical_cores=None,
            memory_mib=160 * 1024,
            numa_memory_nodes=None,
        )
        with patch(
            "cea.chips.benchpress.packages.ucache_bench.sizing._detect_hardware_values",
            return_value=(90, 54, 1, 1),
        ):
            topology = _resolve_topology(args)

        self.assertEqual(topology, HardwareTopology(90, 54, 160 * 1024))

    def test_explicit_zero_numa_nodes_is_rejected(self) -> None:
        args = Namespace(
            logical_cpus=8,
            physical_cores=4,
            memory_mib=8 * 1024,
            numa_memory_nodes=0,
        )

        with self.assertRaisesRegex(ValueError, "numa_memory_nodes"):
            _resolve_topology(args)

    def test_cache_uses_taobench_style_memory_fraction_after_reserve(self) -> None:
        cases = (
            (4 * 1024, 2 * 1024, 1536),
            (16 * 1024, 8 * 1024, 6 * 1024),
            (64 * 1024, 32 * 1024, 24 * 1024),
            (256 * 1024, 32 * 1024, 168 * 1024),
            (2 * 1024 * 1024, 32 * 1024, 1024 * 1024),
        )
        for memory_mib, reserve_mib, cache_mib in cases:
            with self.subTest(memory_mib=memory_mib):
                self.assertEqual(calculate_reserve_mib(memory_mib), reserve_mib)
                self.assertEqual(calculate_cache_mib(memory_mib), cache_mib)

    def test_cache_is_monotonic_aligned_capped_and_reserved(self) -> None:
        memory_points = (
            8 * 1024,
            16 * 1024,
            64 * 1024,
            256 * 1024,
            768 * 1024,
            2 * 1024 * 1024,
        )
        cache_sizes = [calculate_cache_mib(memory_mib) for memory_mib in memory_points]

        self.assertEqual(cache_sizes, sorted(cache_sizes))
        for memory_mib, cache_mib in zip(memory_points, cache_sizes):
            with self.subTest(memory_mib=memory_mib):
                self.assertEqual(cache_mib % 64, 0)
                self.assertLessEqual(cache_mib, 1024 * 1024)
                self.assertLessEqual(
                    cache_mib + calculate_reserve_mib(memory_mib), memory_mib
                )
        self.assertEqual(cache_sizes[-1], 1024 * 1024)

    def test_hash_power_uses_cache_size_boundaries(self) -> None:
        cases = (
            (64 * 1024, 28),
            (64 * 1024 + 64, 29),
            (256 * 1024, 29),
            (256 * 1024 + 64, 30),
            (512 * 1024, 30),
            (512 * 1024 + 64, 31),
        )
        for cache_mib, expected in cases:
            with self.subTest(cache_mib=cache_mib):
                self.assertEqual(calculate_hash_power(cache_mib), expected)

    def test_item_size_changes_key_count_not_hash_power(self) -> None:
        topology = HardwareTopology(108, 60, 384 * 1024)
        small_items = calculate_resources(topology, average_item_bytes=512)
        large_items = calculate_resources(topology, average_item_bytes=2048)

        self.assertEqual(small_items.key_count, 4 * large_items.key_count)
        self.assertEqual(small_items.hash_power, large_items.hash_power)

    def test_effective_cores_are_diagnostic_and_io_uses_logical_cpus(self) -> None:
        topology = HardwareTopology(108, 60, 192 * 1024)
        resources = calculate_resources(topology)

        self.assertEqual(topology.effective_cores, 72)
        self.assertTrue(topology.substantial_smt)
        self.assertEqual(resources.io_threads, 108)

    def test_substantial_smt_boundary(self) -> None:
        self.assertFalse(has_substantial_smt(59, 40))
        self.assertTrue(has_substantial_smt(60, 40))

    def test_server_workload_constants(self) -> None:
        resources = calculate_resources(HardwareTopology(104, 64, 320 * 1024))

        self.assertEqual(resources.acceptor_threads, 4)
        self.assertEqual(resources.lock_power, 24)
        self.assertEqual(resources.cache_shards, 524_288)

    def test_process_count_formulas_and_cap(self) -> None:
        self.assertEqual(calculate_total_processes(40, LoadVariant.PRODUCTION), 16)
        self.assertEqual(calculate_total_processes(40, LoadVariant.EXTREME), 16)
        self.assertEqual(calculate_total_processes(200, LoadVariant.PRODUCTION), 28)
        self.assertEqual(calculate_total_processes(200, LoadVariant.EXTREME), 40)
        self.assertEqual(calculate_total_processes(1000, LoadVariant.PRODUCTION), 64)
        self.assertEqual(calculate_total_processes(1000, LoadVariant.EXTREME), 64)

    def test_process_distribution_by_variant(self) -> None:
        topology = HardwareTopology(300, 200, 384 * 1024)
        production = calculate_client_shape(topology, LoadVariant.PRODUCTION)
        extreme = calculate_client_shape(topology, LoadVariant.EXTREME)

        self.assertEqual(production.total_processes, 28)
        self.assertEqual(production.client_hosts, 1)
        self.assertEqual(production.processes_per_host, 28)
        self.assertEqual(extreme.total_processes, 40)
        self.assertEqual(extreme.client_hosts, 5)
        self.assertEqual(extreme.processes_per_host, 8)

    def test_t2_vnc_equation_uses_one_production_client_host(self) -> None:
        for physical_cores, memory_gib, total_processes in (
            (192, 1408, 24),
            (248, 1800, 32),
        ):
            with self.subTest(physical_cores=physical_cores):
                topology = HardwareTopology(
                    logical_cpus=2 * physical_cores,
                    physical_cores=physical_cores,
                    memory_mib=memory_gib * 1024,
                )
                shape = calculate_client_shape(topology, LoadVariant.PRODUCTION)

                self.assertEqual(shape.total_processes, total_processes)
                self.assertEqual(shape.num_proxies, 16)
                self.assertEqual(shape.client_hosts, 1)
                self.assertEqual(shape.processes_per_host, total_processes)

    def test_proxy_count_without_substantial_smt(self) -> None:
        topology = HardwareTopology(119, 80, 192 * 1024)

        self.assertFalse(topology.substantial_smt)
        self.assertEqual(calculate_proxy_count(topology, LoadVariant.PRODUCTION), 8)

    def test_proxy_count_for_memory_rich_smt(self) -> None:
        topology = HardwareTopology(232, 144, 704 * 1024)

        self.assertEqual(calculate_proxy_count(topology, LoadVariant.PRODUCTION), 12)
        self.assertEqual(calculate_proxy_count(topology, LoadVariant.EXTREME), 72)

    def test_proxy_count_for_high_core_smt(self) -> None:
        topology = HardwareTopology(152, 92, 384 * 1024)

        self.assertEqual(calculate_proxy_count(topology, LoadVariant.PRODUCTION), 8)

    def test_proxy_count_for_other_substantial_smt(self) -> None:
        topology = HardwareTopology(88, 48, 192 * 1024)

        self.assertEqual(calculate_proxy_count(topology, LoadVariant.PRODUCTION), 4)

    def test_production_proxy_count_preserves_validated_platform_shapes(self) -> None:
        cases = (
            (52, 26, 4),
            (72, 36, 4),
            (72, 72, 4),
            (176, 88, 12),
            (128, 128, 8),
            (316, 158, 16),
        )
        for logical_cpus, physical_cores, expected in cases:
            with self.subTest(logical_cpus=logical_cpus, physical_cores=physical_cores):
                topology = HardwareTopology(logical_cpus, physical_cores, 384 * 1024)
                self.assertEqual(
                    calculate_proxy_count(topology, LoadVariant.PRODUCTION), expected
                )

    def test_proxy_count_clamps_to_destination_safe_range(self) -> None:
        small = HardwareTopology(8, 4, 512 * 1024)
        large = HardwareTopology(1200, 800, 384 * 1024)

        self.assertEqual(calculate_proxy_count(small, LoadVariant.PRODUCTION), 4)
        self.assertEqual(calculate_proxy_count(large, LoadVariant.PRODUCTION), 16)

    def test_round_nearest_uses_half_up(self) -> None:
        self.assertEqual(_round_to_nearest_multiple(10, 4), 12)
        self.assertEqual(_round_to_nearest_multiple(9, 4), 8)

    def test_connection_target_scales_until_the_measured_cap(self) -> None:
        small = HardwareTopology(16, 8, 64 * 1024)
        medium = HardwareTopology(96, 64, 256 * 1024)
        large = HardwareTopology(512, 256, 1024 * 1024)

        self.assertLess(
            calculate_target_connections(small),
            calculate_target_connections(medium),
        )
        self.assertLess(
            calculate_target_connections(medium),
            calculate_target_connections(large),
        )
        self.assertEqual(calculate_target_connections(large), 220_000)

    def test_connections_are_variant_aligned_and_bounded(self) -> None:
        physical_core_counts = (12, 24, 48, 72, 96, 144, 220, 330, 385, 800)
        memory_sizes_mib = (
            64 * 1024,
            192 * 1024,
            384 * 1024,
            512 * 1024 - 1,
            512 * 1024,
            704 * 1024,
        )
        for physical_cores in physical_core_counts:
            logical_cpu_counts = (
                physical_cores,
                (5 * physical_cores + 3) // 4,
                (3 * physical_cores + 1) // 2,
                2 * physical_cores,
            )
            for logical_cpus in logical_cpu_counts:
                for memory_mib in memory_sizes_mib:
                    topology = HardwareTopology(
                        logical_cpus,
                        physical_cores,
                        memory_mib,
                    )
                    with self.subTest(topology=topology):
                        production = calculate_client_shape(
                            topology, LoadVariant.PRODUCTION
                        )
                        extreme = calculate_client_shape(topology, LoadVariant.EXTREME)

                        target_connections = calculate_target_connections(topology)
                        for shape in (production, extreme):
                            self.assertGreaterEqual(
                                shape.total_connections, target_connections
                            )
                            self.assertLess(
                                shape.total_connections,
                                target_connections + shape.connection_quantum,
                            )
                            self.assertEqual(
                                shape.total_connections % shape.connection_quantum,
                                0,
                            )
                            self.assertLessEqual(
                                shape.total_connections,
                                shape.max_total_connections,
                            )

    def test_memory_rich_smt_aligns_each_variant_independently(self) -> None:
        topology = HardwareTopology(232, 144, 704 * 1024)
        production = calculate_client_shape(topology, LoadVariant.PRODUCTION)
        extreme = calculate_client_shape(topology, LoadVariant.EXTREME)

        self.assertEqual(production.num_proxies, 12)
        self.assertEqual(extreme.num_proxies, 72)
        self.assertEqual(production.total_connections, 220_080)
        self.assertEqual(extreme.total_connections, 221_184)
        self.assertEqual(
            production.total_connections % production.connection_quantum, 0
        )
        self.assertEqual(extreme.total_connections % extreme.connection_quantum, 0)

    def test_connections_respect_aligned_per_process_limit(self) -> None:
        topology = HardwareTopology(20_000, 10_000, 640 * 1024)

        for variant in LoadVariant:
            with self.subTest(variant=variant):
                connections = calculate_total_connections(topology, variant)
                shape = calculate_client_shape(topology, variant)
                self.assertEqual(connections, 220_160)
                self.assertEqual(connections % shape.connection_quantum, 0)
                self.assertLessEqual(
                    connections // shape.total_processes,
                    32_768,
                )

    def test_connections_fail_when_process_cap_cannot_reach_target(self) -> None:
        topology = HardwareTopology(8, 4, 8 * 1024)

        with (
            patch(
                "cea.chips.benchpress.packages.ucache_bench.sizing._DESTINATIONS_PER_PROCESS",
                1_024,
            ),
            self.assertRaisesRegex(
                ValueError,
                "cannot reach 180096 total connections without exceeding 1024",
            ),
        ):
            calculate_total_connections(topology, LoadVariant.PRODUCTION)

    def test_workload_defaults_are_variant_stable(self) -> None:
        topology = HardwareTopology(152, 92, 384 * 1024)
        production = recommend(topology, LoadVariant.PRODUCTION, aggregate_qps=32_000)
        extreme = recommend(topology, LoadVariant.EXTREME, aggregate_qps=32_000)

        common = {
            "rpc_num_cpu_worker_threads": 1,
            "num_threads": 8,
            "max_inflight": 150,
            "warmup_max_inflight": 32,
            "warmup_seconds": 720,
            "duration_seconds": 240,
            "timeout_seconds": 2400,
            "connection_ramp_seconds": 25,
            "open_loop_refill_on_miss": 0,
        }
        for key, value in common.items():
            with self.subTest(key=key):
                self.assertEqual(production.params[key], value)
                self.assertEqual(extreme.params[key], value)
        self.assertEqual(production.params["open_loop_max_outstanding"], 8192)
        self.assertEqual(production.params["open_loop_max_lateness_us"], 500_000)
        self.assertEqual(extreme.params["open_loop_max_outstanding"], 8192)
        self.assertEqual(extreme.params["open_loop_max_lateness_us"], 500_000)
        self.assertEqual(production.params["failures_until_tko"], 12)
        self.assertEqual(extreme.params["failures_until_tko"], 12)
        self.assertEqual(production.params["server_numa_interleave"], 0)
        self.assertEqual(extreme.params["server_numa_interleave"], 0)

        numa = recommend(
            HardwareTopology(316, 158, 1024 * 1024, numa_memory_nodes=2),
            LoadVariant.PRODUCTION,
            aggregate_qps=3_600_000,
        )
        self.assertEqual(numa.params["server_numa_interleave"], 1)

    def test_missing_qps_params_only_fails_clearly(self) -> None:
        stderr = StringIO()
        with redirect_stderr(stderr), self.assertRaises(SystemExit) as context:
            main(
                [
                    "--logical-cpus=108",
                    "--physical-cores=60",
                    f"--memory-mib={192 * 1024}",
                    "--params-only",
                ]
            )

        self.assertEqual(context.exception.code, 2)
        self.assertIn(
            "requires --aggregate-qps or --baseline-latency-us", stderr.getvalue()
        )

    def test_qps_below_process_count_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "derived client process count"):
            recommend(
                HardwareTopology(108, 60, 192 * 1024),
                LoadVariant.PRODUCTION,
                aggregate_qps=4,
            )

    def test_qps_is_measurement_driven(self) -> None:
        topology = HardwareTopology(108, 60, 192 * 1024)
        without_qps = recommend(topology, LoadVariant.PRODUCTION)
        supplied = recommend(
            topology,
            LoadVariant.PRODUCTION,
            aggregate_qps=24_000,
        )

        self.assertNotIn("open_loop_qps", without_qps.params)
        self.assertNotIn("process_ramp_seconds", without_qps.params)
        self.assertEqual(
            without_qps.to_dict()["calibration"],
            {
                "required": True,
                "seeded_from_baseline_latency": False,
                "target_cpu": 0.60,
            },
        )
        self.assertEqual(supplied.aggregate_qps, 24_000)
        self.assertEqual(supplied.params["open_loop_qps"], 1500)
        self.assertEqual(supplied.params["process_ramp_seconds"], 64)

    def test_latency_seed_and_next_qps_remain_bounded(self) -> None:
        self.assertEqual(seed_qps(90, 1000), 9000)
        self.assertEqual(next_qps(1000, observed_cpu=0.10, target_cpu=0.80), 1500)
        self.assertEqual(next_qps(1000, observed_cpu=1.00, target_cpu=0.60), 670)
        self.assertEqual(next_qps(1000, observed_cpu=0.50, target_cpu=0.60), 1200)

    def test_output_is_orchestration_neutral(self) -> None:
        recommendation = recommend(
            HardwareTopology(108, 60, 192 * 1024),
            LoadVariant.PRODUCTION,
            aggregate_qps=24_000,
        )
        output = recommendation.to_dict()

        self.assertEqual(output["benchmark_params"], recommendation.params)
        self.assertEqual(
            set(output),
            {"variant", "hardware", "derived", "calibration", "benchmark_params"},
        )
