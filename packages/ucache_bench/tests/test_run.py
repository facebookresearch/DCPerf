# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from cea.chips.benchpress.packages.ucache_bench.run import (
    affinitize_nic,
    ClientSummary,
    CommandResult,
    even_rss_channel_count,
    init_parser,
    parse_client_summary,
    parse_rss_ring_entries,
    run_client,
    run_cmd,
    run_server,
    validate_client_summary,
    validate_executable,
)


_VALID_OUTPUT: str = """
=== UcacheBench Results ===
WARMUP PHASE:
  Status: ✓ SUCCESS
  Duration: 1.00 seconds
  Operations: 10 (10.0 QPS)
  SET Successes: 10
  SET Errors: 0
  SET Errors (warmup tail): 0
  Success Rate: 100.0%

BENCHMARK PHASE:
  Duration: 2.00 seconds
  Total Operations: 100
  QPS: 50.0

GET Operations: 60
  Hits: 40
  Misses: 19
  Errors: 1
  Hit Ratio: 66.67%

SET Operations: 40
  Successes: 39
  Errors: 1

Latency Percentiles (ms):
  P50: 0.10
  P95: 0.20
  P99: 0.30
  P99.9: 0.40
"""


class CommandResultTest(unittest.TestCase):
    def test_child_return_code_is_propagated(self) -> None:
        result = run_cmd([sys.executable, "-c", "raise SystemExit(7)"])

        self.assertEqual(result.returncode, 7)
        with self.assertRaises(subprocess.CalledProcessError):
            result.check()

    def test_timeout_is_propagated(self) -> None:
        result = run_cmd(
            [sys.executable, "-c", "import time; time.sleep(10)"], timeout=0.05
        )

        self.assertTrue(result.timed_out)
        with self.assertRaises(subprocess.TimeoutExpired):
            result.check()

    def test_dry_run_is_successful(self) -> None:
        result = run_cmd(["not-executed"], for_real=False)

        self.assertEqual(result, CommandResult(["not-executed"], "", 0, False, None))


class ClientSummaryTest(unittest.TestCase):
    def test_valid_summary_passes_structural_validation(self) -> None:
        summary = parse_client_summary(_VALID_OUTPUT)

        validate_client_summary(summary)
        self.assertEqual(summary.total_operations, 100)
        self.assertEqual(summary.get_hits, 40)
        self.assertEqual(summary.set_successes, 39)

    def test_zero_operations_fails(self) -> None:
        summary = ClientSummary(
            warmup_operations=0,
            warmup_set_successes=0,
            warmup_set_errors=0,
            warmup_tail_set_errors=0,
            total_operations=0,
            qps=0.0,
            get_operations=0,
            get_hits=0,
            get_misses=0,
            get_errors=0,
            set_operations=0,
            set_successes=0,
            set_errors=0,
            latencies_ms=(0.0, 0.0, 0.0, 0.0),
        )

        with self.assertRaisesRegex(ValueError, "no operations"):
            validate_client_summary(summary)

    def test_all_error_output_fails(self) -> None:
        summary = ClientSummary(
            warmup_operations=10,
            warmup_set_successes=0,
            warmup_set_errors=10,
            warmup_tail_set_errors=10,
            total_operations=10,
            qps=10.0,
            get_operations=5,
            get_hits=0,
            get_misses=0,
            get_errors=5,
            set_operations=5,
            set_successes=0,
            set_errors=5,
            latencies_ms=(0.0, 0.0, 0.0, 0.0),
        )

        with self.assertRaisesRegex(ValueError, "no successful protocol responses"):
            validate_client_summary(summary)

    def test_inconsistent_warmup_accounting_fails(self) -> None:
        summary = replace(parse_client_summary(_VALID_OUTPUT), warmup_operations=11)

        with self.assertRaisesRegex(ValueError, "warmup SET accounting"):
            validate_client_summary(summary)

    def test_inconsistent_accounting_fails(self) -> None:
        summary = parse_client_summary(_VALID_OUTPUT)
        invalid = replace(summary, total_operations=summary.total_operations + 1)

        with self.assertRaisesRegex(ValueError, "GET and SET totals"):
            validate_client_summary(invalid)

    def test_warmup_accepts_recovered_startup_errors(self) -> None:
        summary = replace(
            parse_client_summary(_VALID_OUTPUT),
            warmup_operations=20,
            warmup_set_errors=10,
            warmup_tail_set_errors=0,
        )

        validate_client_summary(summary, require_warmup=True)

    def test_warmup_rejects_errors_in_final_stable_minute(self) -> None:
        summary = replace(
            parse_client_summary(_VALID_OUTPUT),
            warmup_operations=20,
            warmup_set_errors=10,
            warmup_tail_set_errors=1,
        )

        with self.assertRaisesRegex(ValueError, "did not finish"):
            validate_client_summary(summary, require_warmup=True)


class ParserTest(unittest.TestCase):
    def test_subcommand_is_required(self) -> None:
        with self.assertRaises(SystemExit) as context:
            init_parser().parse_args([])

        self.assertEqual(context.exception.code, 2)

    def test_server_rejects_negative_process_ramp(self) -> None:
        args = init_parser().parse_args(["server", "--process-ramp-seconds=-1"])

        with self.assertRaisesRegex(ValueError, "must be non-negative"):
            run_server(args)

    def test_server_numa_interleave_prefixes_numactl(self) -> None:
        args = init_parser().parse_args(
            ["server", "--server-numa-interleave=1", "--interface-name=lo"]
        )
        result = CommandResult([], "", 0, False, None)
        target = "cea.chips.benchpress.packages.ucache_bench.run.run_cmd"

        with (
            patch(target, return_value=result) as run,
            patch(
                "cea.chips.benchpress.packages.ucache_bench.run.shutil.which",
                return_value="/usr/bin/numactl",
            ),
        ):
            run_server(args)

        command = run.call_args.args[0]
        self.assertEqual(command[:2], ["/usr/bin/numactl", "--interleave=all"])
        self.assertTrue(command[2].endswith("/server/ucachebench_server"))

    def test_server_numa_interleave_requires_numactl(self) -> None:
        args = init_parser().parse_args(
            ["server", "--server-numa-interleave=1", "--interface-name=lo"]
        )

        with (
            patch(
                "cea.chips.benchpress.packages.ucache_bench.run.shutil.which",
                return_value=None,
            ),
            self.assertRaisesRegex(RuntimeError, "requires numactl"),
        ):
            run_server(args)

    def test_process_ramp_requires_admin_coordination(self) -> None:
        args = init_parser().parse_args(
            [
                "client",
                "--server-host=cache.example.com",
                "--process-ramp-seconds=16",
            ]
        )

        with self.assertRaisesRegex(ValueError, "requires --admin-port"):
            run_client(args)

    def test_process_ramp_requires_open_loop(self) -> None:
        args = init_parser().parse_args(
            [
                "client",
                "--server-host=cache.example.com",
                "--admin-port=11213",
                "--process-ramp-seconds=16",
            ]
        )

        with self.assertRaisesRegex(ValueError, "requires --open-loop-qps"):
            run_client(args)

    def test_full_load_stabilization_requires_process_ramp(self) -> None:
        args = init_parser().parse_args(
            ["server", "--full-load-stabilization-seconds=60"]
        )

        with self.assertRaisesRegex(ValueError, "requires --process-ramp-seconds"):
            run_server(args)

    def test_full_load_stabilization_is_forwarded_to_server_binary(self) -> None:
        args = init_parser().parse_args(
            [
                "server",
                "--interface-name=lo",
                "--num-clients=16",
                "--process-ramp-seconds=64",
                "--full-load-stabilization-seconds=60",
            ]
        )
        result = CommandResult([], "", 0, False, None)
        target = "cea.chips.benchpress.packages.ucache_bench.run.run_cmd"

        with patch(target, return_value=result) as run:
            run_server(args)

        command = run.call_args.args[0]
        self.assertIn("--process_ramp_seconds=64", command)
        self.assertIn("--full_load_stabilization_seconds=60", command)

    def test_process_ramp_is_forwarded_to_client_binary(self) -> None:
        args = init_parser().parse_args(
            [
                "client",
                "--server-host=cache.example.com",
                "--admin-port=11213",
                "--process-ramp-seconds=16",
                "--open-loop-qps=1000",
            ]
        )
        result = CommandResult([], "", 0, False, None)
        target = "cea.chips.benchpress.packages.ucache_bench.run.run_cmd"

        with patch(target, return_value=result) as run:
            run_client(args)

        command = run.call_args.args[0]
        self.assertIn("--admin_port=11213", command)
        self.assertIn("--process_ramp_seconds=16", command)

    def test_warmup_cap_keeps_adaptive_ramp_enabled(self) -> None:
        args = init_parser().parse_args(
            [
                "client",
                "--server-host=cache.example.com",
                "--warmup-max-inflight=32",
            ]
        )
        result = CommandResult([], "", 0, False, None)
        target = "cea.chips.benchpress.packages.ucache_bench.run.run_cmd"

        with patch(target, return_value=result) as run:
            run_client(args)

        command = run.call_args.args[0]
        self.assertIn("--warmup_max_inflight=32", command)
        self.assertNotIn("--warmup_adaptive_load=false", command)

    def test_even_rss_channel_count_rounds_down_to_power_of_two(self) -> None:
        cases = {0: 0, 1: 1, 63: 32, 64: 64, 126: 64, 128: 128, 252: 128}
        for requested, expected in cases.items():
            with self.subTest(requested=requested):
                self.assertEqual(even_rss_channel_count(requested), expected)

    def test_parse_rss_ring_entries_counts_wrapped_ring(self) -> None:
        rows = [
            f"{start:5}: " + " ".join(f"{(start + i) % 63:5}" for i in range(8))
            for start in range(0, 64, 8)
        ]
        output = "\n".join(
            ["RX flow hash indirection table for eth0 with 63 RX ring(s):"]
            + rows
            + ["RSS hash key:", "75:d7:bd:e5:43:70:df:26", "RSS hash function:"]
        )

        entries = parse_rss_ring_entries(output)

        self.assertEqual(len(entries), 63)
        self.assertEqual(sum(entries.values()), 64)
        self.assertEqual(entries[0], 2)
        self.assertEqual({entries[ring] for ring in range(1, 63)}, {1})

    def _run_affinitize(
        self, cpus: int, ratio: float, current_channels: int
    ) -> list[list[str]]:
        args = init_parser().parse_args(
            [
                "client",
                "--server-host=cache.example.com",
                f"--nic-channel-ratio={ratio}",
            ]
        )
        channels = (
            "Channel parameters for eth0:\nPre-set maximums:\nCombined:\t128\n"
            f"Current hardware settings:\nCombined:\t{current_channels}\n"
        )
        commands: list[list[str]] = []

        def fake_run(cmd: list[str], **kwargs: object) -> subprocess.CompletedProcess:
            commands.append(cmd)
            stdout = channels if cmd[:2] == ["ethtool", "-l"] else ""
            return subprocess.CompletedProcess(cmd, 0, stdout, "")

        module = "cea.chips.benchpress.packages.ucache_bench.run"
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch(f"{module}.NIC_SETUP_LOCK_PATH", f"{tmp}/nic.lock"),
            patch(f"{module}.os.sched_getaffinity", return_value=set(range(cpus))),
            patch(f"{module}.subprocess.run", side_effect=fake_run),
        ):
            affinitize_nic(args)
        return commands

    def test_affinitize_nic_uses_power_of_two_channels(self) -> None:
        commands = self._run_affinitize(cpus=252, ratio=0.5, current_channels=63)

        self.assertIn(["ethtool", "-L", "eth0", "combined", "64"], commands)
        affinitize = next(c for c in commands if "--max-cpus" in c)
        self.assertEqual(affinitize[affinitize.index("--max-cpus") + 1], "64")

    def test_affinitize_nic_keeps_matching_channel_count(self) -> None:
        commands = self._run_affinitize(cpus=252, ratio=1.0, current_channels=128)

        self.assertFalse([c for c in commands if c[:2] == ["ethtool", "-L"]])
        self.assertTrue([c for c in commands if "--max-cpus" in c])

    def test_client_configures_nic_only_when_ratio_is_set(self) -> None:
        result = CommandResult([], "", 0, False, None)
        module = "cea.chips.benchpress.packages.ucache_bench.run"
        for ratio, expected_calls in ((0.0, 0), (1.0, 1)):
            args = init_parser().parse_args(
                [
                    "client",
                    "--server-host=cache.example.com",
                    f"--nic-channel-ratio={ratio}",
                ]
            )
            with (
                self.subTest(ratio=ratio),
                patch(f"{module}.run_cmd", return_value=result),
                patch(f"{module}.affinitize_nic") as configure,
            ):
                run_client(args)
                self.assertEqual(configure.call_count, expected_calls)

    def test_missing_or_non_executable_binary_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing"
            non_executable = Path(tmp) / "client"
            non_executable.write_text("binary")

            with self.assertRaises(FileNotFoundError):
                validate_executable(missing)
            with self.assertRaises(PermissionError):
                validate_executable(non_executable)
