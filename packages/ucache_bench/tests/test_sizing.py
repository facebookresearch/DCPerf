# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from cea.chips.benchpress.packages.ucache_bench.sizing import (
    _parse_cpu_list,
    calculate_cache_mb,
    calculate_hash_power,
    detect_hardware,
    find_calibration,
    HardwareTopology,
    LoadVariant,
    recommend,
)


class SizingTest(unittest.TestCase):
    def test_parse_cpu_list(self) -> None:
        self.assertEqual(_parse_cpu_list("0-3,8,10-11"), {0, 1, 2, 3, 8, 10, 11})

    def test_detect_hardware_respects_affinity_and_siblings(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for cpu, siblings in ((0, "0,2"), (1, "1,3"), (2, "0,2"), (3, "1,3")):
                path = (
                    root
                    / "sys"
                    / "devices"
                    / "system"
                    / "cpu"
                    / f"cpu{cpu}"
                    / "topology"
                )
                path.mkdir(parents=True)
                (path / "thread_siblings_list").write_text(siblings)
            meminfo = root / "meminfo"
            meminfo.write_text("MemTotal:       65536000 kB\n")
            topology = detect_hardware(
                sysfs_root=root / "sys", meminfo_path=meminfo, affinity={0, 2, 3}
            )
        self.assertEqual(topology.logical_cpus, 3)
        self.assertEqual(topology.physical_cores, 2)
        self.assertEqual(topology.memory_mib, 64_000)

    def test_known_hardware_matches_calibration(self) -> None:
        expected = {
            "T2_TRN": HardwareTopology(316, 158, 1_034_736),
            "T1_BGM": HardwareTopology(176, 88, 257_120),
            "T11_GRC_ARM": HardwareTopology(72, 72, 260_682),
            "T1_MLN": HardwareTopology(72, 36, 62_000),
            "T1_CPL": HardwareTopology(52, 26, 62_000),
        }
        for name, topology in expected.items():
            with self.subTest(name=name):
                calibration = find_calibration(topology)
                self.assertIsNotNone(calibration)
                self.assertEqual(calibration.name if calibration else None, name)

    def test_production_recommendations_reproduce_measured_matrix(self) -> None:
        cases = (
            (HardwareTopology(316, 158, 1_034_736), 820_000, 5_000_000, 20, 1),
            (HardwareTopology(176, 88, 257_120), 170_000, 2_500_000, 16, 1),
            (HardwareTopology(72, 72, 260_682), 160_000, 1_500_000, 16, 1),
            (HardwareTopology(72, 36, 62_000), 24_000, 700_000, 16, 1),
            (HardwareTopology(52, 26, 62_000), 24_000, 580_000, 16, 1),
        )
        for topology, cache_mb, qps, processes, hosts in cases:
            with self.subTest(topology=topology):
                result = recommend(topology, LoadVariant.PRODUCTION)
                self.assertEqual(result.source, "calibrated")
                self.assertEqual(result.params["memory_mb"], cache_mb)
                self.assertEqual(result.aggregate_qps, qps)
                self.assertEqual(result.total_client_processes, processes)
                self.assertEqual(result.params["num_client_hosts"], hosts)

    def test_extreme_recommendations_reproduce_measured_matrix(self) -> None:
        cases = (
            (HardwareTopology(316, 158, 1_034_736), 4_800_000, 32, 4, 401_920),
            (HardwareTopology(176, 88, 257_120), 2_800_000, 16, 2, 133_440),
            (HardwareTopology(72, 72, 260_682), 1_700_000, 16, 2, 40_000),
            (HardwareTopology(72, 36, 62_000), 1_000_000, 16, 2, 51_520),
            (HardwareTopology(52, 26, 62_000), 780_000, 16, 2, 37_120),
        )
        for topology, qps, processes, hosts, connections in cases:
            with self.subTest(topology=topology):
                result = recommend(topology, LoadVariant.EXTREME)
                self.assertEqual(result.aggregate_qps, qps)
                self.assertEqual(result.total_client_processes, processes)
                self.assertEqual(result.params["num_client_hosts"], hosts)
                self.assertEqual(result.total_connections, connections)

    def test_formula_fallback_distinguishes_smt(self) -> None:
        smt = recommend(HardwareTopology(96, 48, 128_000), LoadVariant.EXTREME)
        no_smt = recommend(HardwareTopology(48, 48, 128_000), LoadVariant.EXTREME)
        self.assertEqual(smt.source, "formula")
        self.assertEqual(no_smt.source, "formula")
        self.assertGreater(smt.aggregate_qps, no_smt.aggregate_qps)
        self.assertGreater(
            int(smt.params["num_proxies"]), int(no_smt.params["num_proxies"])
        )

    def test_calibration_rejects_materially_different_capacity(self) -> None:
        low_memory_t2 = HardwareTopology(316, 158, 776_052)
        reduced_milan_cpuset = HardwareTopology(64, 32, 62_000)
        self.assertIsNone(find_calibration(low_memory_t2))
        self.assertIsNone(find_calibration(reduced_milan_cpuset))
        result = recommend(low_memory_t2, LoadVariant.PRODUCTION)
        self.assertEqual(result.source, "formula")
        self.assertLess(int(result.params["memory_mb"]), low_memory_t2.memory_mib)

    def test_partial_smt_does_not_change_formula_class(self) -> None:
        no_smt = recommend(HardwareTopology(65, 65, 128_000), LoadVariant.PRODUCTION)
        one_sibling = recommend(
            HardwareTopology(66, 65, 128_000), LoadVariant.PRODUCTION
        )
        self.assertEqual(no_smt.aggregate_qps, one_sibling.aggregate_qps)
        self.assertEqual(
            no_smt.params["num_proxies"], one_sibling.params["num_proxies"]
        )

    def test_formula_caps_destinations_per_process(self) -> None:
        result = recommend(
            HardwareTopology(576, 288, 1_048_576), LoadVariant.PRODUCTION
        )
        proxies = int(result.params["num_proxies"])
        fanout = int(result.params["additional_fanout"])
        self.assertLessEqual(proxies * (fanout + 1), 32_768)

    def test_formula_cache_is_monotonic_across_boundaries(self) -> None:
        memory_points = (47_999, 48_000, 48_001, 255_999, 256_000, 256_001, 524_288)
        cache_sizes = [
            calculate_cache_mb(HardwareTopology(16, 8, memory_mib))
            for memory_mib in memory_points
        ]
        self.assertEqual(cache_sizes, sorted(cache_sizes))
        for memory_mib, cache_mb in zip(memory_points, cache_sizes):
            with self.subTest(memory_mib=memory_mib):
                self.assertLessEqual(cache_mb, round(memory_mib * 0.8) + 500)

    def test_formula_cache_and_hash_power_boundaries(self) -> None:
        small = HardwareTopology(16, 8, 32_000)
        medium = HardwareTopology(128, 64, 256_000)
        large = HardwareTopology(256, 128, 1_024_000)
        self.assertEqual(calculate_cache_mb(small), 16_000)
        self.assertEqual(calculate_hash_power(calculate_cache_mb(small)), 28)
        self.assertEqual(calculate_cache_mb(medium), 170_000)
        self.assertEqual(calculate_hash_power(calculate_cache_mb(medium)), 29)
        self.assertEqual(calculate_cache_mb(large), 811_000)
        self.assertEqual(calculate_hash_power(calculate_cache_mb(large)), 31)

    def test_output_names_required_automark_adapter(self) -> None:
        result = recommend(
            HardwareTopology(52, 26, 62_000), LoadVariant.PRODUCTION
        ).to_dict()
        self.assertEqual(result["automark_benchmark"], "ucache_bench_debug")


if __name__ == "__main__":
    unittest.main()
