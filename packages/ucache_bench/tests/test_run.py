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
    ClientSummary,
    CommandResult,
    init_parser,
    parse_client_summary,
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

    def test_inconsistent_accounting_fails(self) -> None:
        summary = parse_client_summary(_VALID_OUTPUT)
        invalid = replace(summary, total_operations=summary.total_operations + 1)

        with self.assertRaisesRegex(ValueError, "GET and SET totals"):
            validate_client_summary(invalid)


class ParserTest(unittest.TestCase):
    def test_subcommand_is_required(self) -> None:
        with self.assertRaises(SystemExit) as context:
            init_parser().parse_args([])

        self.assertEqual(context.exception.code, 2)

    def test_server_rejects_negative_process_ramp(self) -> None:
        args = init_parser().parse_args(["server", "--process-ramp-seconds=-1"])

        with self.assertRaisesRegex(ValueError, "must be non-negative"):
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

    def test_missing_or_non_executable_binary_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing"
            non_executable = Path(tmp) / "client"
            non_executable.write_text("binary")

            with self.assertRaises(FileNotFoundError):
                validate_executable(missing)
            with self.assertRaises(PermissionError):
                validate_executable(non_executable)
