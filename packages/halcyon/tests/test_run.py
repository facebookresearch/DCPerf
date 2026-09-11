#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import io
import json
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

import run as halcyon_run


class RunTest(unittest.TestCase):
    def _paired_summary(self) -> dict[str, object]:
        return {
            "file_qps": 30.0,
            "file_read_qps": 10.0,
            "file_write_qps": 20.0,
            "file_xput": 3_000_000.0,
            "file_read_xput": 1_000_000.0,
            "file_write_xput": 2_000_000.0,
            "file_latency": 3000.0,
            "file_read_latency": 2000.0,
            "file_write_latency": 4000.0,
            "cpu_util": 50.0,
            "disk_util": 60.0,
            "net_util": 70.0,
            "reactor_useful_busy_ns": 2_000_000.0,
            "reactor_useful_idle_ns": 1_000_000.0,
            "reactor_useful_cpu_ratio": 2.0 / 3.0,
        }

    @patch("run.cleanup_directories")
    @patch("run.resolve_mounts")
    @patch("run.discover_mounts", return_value=[])
    @patch("run.parser")
    def test_partner_only_cleanup_does_not_require_local_mounts(
        self,
        parser,
        discover_mounts,
        resolve_mounts,
        cleanup_directories,
    ) -> None:
        parser.return_value.parse_args.return_value = argparse.Namespace(
            action="cleanup-filesets",
            mounts=None,
            partner="partner.example.com",
            partner_mounts="/mnt/hn1",
            ssh_command="ssh",
        )

        def resolve(value, host, ssh_command):
            if host is None:
                raise RuntimeError("no local mounts")
            return [value]

        resolve_mounts.side_effect = resolve

        try:
            returncode = halcyon_run.main()
        except RuntimeError as error:
            self.fail(f"partner-only cleanup required local mounts: {error}")

        self.assertEqual(returncode, 0)
        discover_mounts.assert_called_once_with(None, "ssh")
        resolve_mounts.assert_called_once_with("/mnt/hn1", "partner.example.com", "ssh")
        cleanup_directories.assert_called_once_with(
            ["/mnt/hn1"], "partner.example.com", "ssh"
        )

    def test_parse_standalone_output_matches_exact_total_label(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "result.txt"
            output.write_text(
                "Totals " + " ".join(["9"] * 12) + "\n"
                "Total 10 20 1 2 3 4 5 6 7 8 50 60\n"
            )

            metrics = halcyon_run.parse_standalone_output(output)

        self.assertEqual(metrics["qps"], 30.0)

    def test_standalone_output_omits_unpublished_metrics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "result.txt"
            output.write_text(
                "Total 10 20 1 2 3 4 5 6 7 8 50 60\n"
                "Per-pool CPU (total): qio_total_cpu_ms=5 "
                "qio_checksum_cpu_ms=2 qflush_total_cpu_ms=3\n"
            )

            metrics = halcyon_run.parse_standalone_output(output)

        self.assertNotIn("reactor_checksum_cpu_ms", metrics)
        self.assertNotIn("qflush_total_cpu_ms", metrics)

    def test_paired_metrics_rejects_missing_values(self) -> None:
        summary = self._paired_summary()
        del summary["file_latency"]

        with self.assertRaisesRegex(ValueError, "file_latency"):
            halcyon_run.paired_metrics(summary)

    def test_paired_metrics_rejects_null_values(self) -> None:
        summary = self._paired_summary()
        summary["file_latency"] = None

        with self.assertRaisesRegex(ValueError, "file_latency"):
            halcyon_run.paired_metrics(summary)

    def test_paired_metrics_names_accounted_reactor_time(self) -> None:
        metrics = halcyon_run.paired_metrics(self._paired_summary())

        self.assertEqual(metrics["reactor_accounted_cpu_ms"], 3.0)
        self.assertNotIn("reactor_total_cpu_ms", metrics)

    @patch("run.stop_daemons")
    @patch("run.run_with_term_timeout", return_value=0)
    @patch("run.start_daemons", return_value=(object(), 123))
    @patch("run.paired_config", return_value={})
    @patch("run.prepare_directories")
    @patch("run.resolve_mounts", return_value=["/mnt/hn1"])
    def test_paired_success_without_output_emits_partial_envelope(
        self,
        resolve_mounts,
        prepare_directories,
        paired_config,
        start_daemons,
        run_with_term_timeout,
        stop_daemons,
    ) -> None:
        del resolve_mounts, prepare_directories, paired_config
        del start_daemons, run_with_term_timeout, stop_daemons
        with tempfile.TemporaryDirectory() as tmp:
            args = self._paired_args(str(Path(tmp) / "missing.json"))
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                returncode = halcyon_run.paired(args, prepare=False)

        self.assertEqual(returncode, 0)
        result = json.loads(stdout.getvalue().removeprefix("HALCYON_RESULT="))
        self.assertTrue(result["halcyon_result"]["partial"])

    @patch("run.stop_daemons")
    @patch("run.run_with_term_timeout", return_value=0)
    @patch("run.start_daemons", return_value=(object(), 123))
    @patch("run.paired_config", return_value={})
    @patch("run.prepare_directories")
    @patch("run.resolve_mounts", return_value=["/mnt/hn1"])
    def test_paired_malformed_output_emits_partial_envelope(
        self,
        resolve_mounts,
        prepare_directories,
        paired_config,
        start_daemons,
        run_with_term_timeout,
        stop_daemons,
    ) -> None:
        del resolve_mounts, prepare_directories, paired_config
        del start_daemons, run_with_term_timeout, stop_daemons
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "malformed.json"
            output.write_text("{broken")
            args = self._paired_args(str(output))
            stdout = io.StringIO()
            try:
                with redirect_stdout(stdout):
                    returncode = halcyon_run.paired(args, prepare=False)
            except json.JSONDecodeError as error:
                self.fail(f"malformed paired output escaped: {error}")

        self.assertEqual(returncode, 0)
        result = json.loads(stdout.getvalue().removeprefix("HALCYON_RESULT="))
        self.assertTrue(result["halcyon_result"]["partial"])

    @patch("run._run")
    @patch("run.install_root", return_value=Path("/tmp/halcyon"))
    @patch("run.prepare_directories")
    @patch("run.resolve_mounts", return_value=["/mnt/hn1"])
    def test_standalone_streams_process_output(
        self, resolve_mounts, prepare_directories, install_root, run_command
    ) -> None:
        del resolve_mounts, prepare_directories, install_root
        run_command.return_value = subprocess.CompletedProcess([], 0)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "result.txt"
            output.write_text("Total 10 20 1 2 3 4 5 6 7 8 50 60\n")
            args = argparse.Namespace(
                mounts=None,
                ssh_command="ssh",
                threads_per_disk=5,
                metadata_backend="manifest",
                warmup=1,
                runtime=1,
                output=str(output),
            )

            halcyon_run.standalone(args, prepare=False)

        self.assertNotIn("capture", run_command.call_args.kwargs)

    def _paired_args(self, output: str) -> argparse.Namespace:
        return argparse.Namespace(
            partner="partner.example.com",
            sender_host="sender.example.com",
            mounts=None,
            partner_mounts=None,
            ssh_command="ssh",
            config=None,
            threads_per_disk=5,
            qnet_threads=16,
            qmeta_threads=0,
            qflush_threads=0,
            metadata_backend="manifest",
            warmup=1,
            runtime=1,
            output=output,
        )
