#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import csv
import json
import os
import re
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from feedsim_report import build_report, public_metrics, read_instance

ROOT = None


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def write_result(self, successful=80, failed=10, pending=10, latency=650):
        (self.root / "inst1.log").write_text(
            "Searching for QPS where 95p latency <= 700 msec\n"
        )
        row = dict(
            sent_requests=100,
            successful_requests=successful,
            failed_requests=failed,
            pending_requests=pending,
            server_errors=failed,
            transport_errors=0,
            elapsed_secs=10,
            requested_qps=12,
            **{"95p_ms": latency},
        )
        with (self.root / "result1.txt").open("w") as output:
            writer = csv.DictWriter(output, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        return read_instance(self.root / "inst1.log", self.root / "result1.txt", False)[
            0
        ]

    def test_report_command_line(self):
        self.write_result(successful=90, failed=0)
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("feedsim_report.py")),
                "--log-prefix",
                str(self.root / "inst"),
                "--csv-prefix",
                str(self.root / "result"),
                "--instances",
                "1",
                "--debug-log",
                str(self.root / "debug.log"),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        report = json.loads(result.stdout)
        self.assertEqual(
            set(report),
            {
                "1",
                "overall",
                "target_percentile",
                "target_latency_msec",
                "spawned_instances",
                "successful_instances",
                "min_qps",
                "max_qps",
                "is_fixed_qps",
                "dr_trace",
            },
        )
        self.assertEqual(
            set(report["1"]),
            {
                "final_requested_qps",
                "final_achieved_qps",
                "final_latency_msec",
            },
        )
        self.assertEqual(
            set(report["overall"]),
            {
                "final_requested_qps",
                "final_achieved_qps",
                "average_latency_msec",
            },
        )
        self.assertEqual(report["spawned_instances"], "1")
        self.assertEqual(report["target_latency_msec"], "700")
        self.assertEqual(report["overall"]["final_achieved_qps"], 9)
        debug = json.loads((self.root / "debug.log").read_text())
        self.assertTrue(debug["benchmark_valid"])
        self.assertEqual(debug["overall"]["successful_requests"], 90)

    def test_success_failure_pending_are_separate(self):
        result = self.write_result()
        self.assertEqual(result["successful_qps"], 8)
        self.assertEqual(result["failed_qps"], 1)
        self.assertAlmostEqual(result["failure_rate_pct"], 100 / 9)
        self.assertEqual(result["completion_rate_pct"], 90)
        self.assertEqual(result["validity_flags"], ["request_failures"])

    def test_final_sla_is_independently_validated(self):
        result = self.write_result(successful=90, failed=0, latency=701)
        self.assertEqual(result["validity_flags"], ["sla_failed"])
        result = self.write_result(successful=90, failed=0, latency=699)
        self.assertTrue(result["benchmark_valid"])

    def test_bad_counts_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "do not reconcile"):
            self.write_result(successful=99)

    def test_missing_instance_is_not_valid(self):
        self.write_result(successful=90, failed=0)
        report = build_report(self.root / "inst", self.root / "result", 2)
        self.assertFalse(report["benchmark_valid"])
        self.assertEqual(report["completed_instances"], 1)
        self.assertEqual(len(report["reporting_errors"]), 1)

    def test_all_missing_measurements_keep_schema_and_diagnostics(self):
        report = build_report(self.root / "inst", self.root / "result", 2)
        report["dr_trace"] = 0
        metrics = public_metrics(report)
        self.assertEqual(metrics["target_percentile"], "95p")
        self.assertEqual(metrics["target_latency_msec"], "")
        self.assertEqual(metrics["successful_instances"], 0)
        self.assertEqual(len(report["reporting_errors"]), 2)

    def test_missing_outcomes_have_explicit_diagnostic(self):
        self.write_result(successful=90, failed=0)
        csv_path = self.root / "result1.txt"
        csv_path.write_text(
            csv_path.read_text().replace("100,90,0,10,0,0,10", ",,,,,,,")
        )
        report = build_report(self.root / "inst", self.root / "result", 1)
        self.assertFalse(report["benchmark_valid"])
        self.assertIn("Missing request outcomes", report["reporting_errors"][0])

    def test_failed_measurement_returns_nonzero_and_preserves_debug_log(self):
        self.write_result()
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("feedsim_report.py")),
                "--log-prefix",
                str(self.root / "inst"),
                "--csv-prefix",
                str(self.root / "result"),
                "--instances",
                "1",
                "--debug-log",
                str(self.root / "debug.log"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["overall"]["final_achieved_qps"], 8)
        self.assertIn(
            "request_failures",
            json.loads((self.root / "debug.log").read_text())["1"]["validity_flags"],
        )


def response_server(drop, duplicate):
    class Handler(socketserver.BaseRequestHandler):
        def read_exact(self, size):
            data = b""
            while len(data) < size:
                more = self.request.recv(size - len(data))
                if not more:
                    return None
                data += more
            return data

        def handle(self):
            index = 0
            try:
                while True:
                    data = self.read_exact(24)
                    if data is None:
                        return
                    kind, request_id, started, size = struct.unpack("!IQQI", data)
                    if size and self.read_exact(size) is None:
                        return
                    if drop:
                        return
                    index += 1
                    # Both replies have zero payload: only explicit status distinguishes them.
                    flag = (1 << 31) if index % 2 else 0
                    response = struct.pack(
                        "!IQQQI", kind | flag, request_id, started, 0, 0
                    )
                    self.request.sendall(response)
                    if duplicate:
                        self.request.sendall(response)
            except OSError:
                return

    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True

    return Server(("127.0.0.1", 0), Handler)


@unittest.skipIf(ROOT is None, "--feedsim-root is required for native tests")
class NativeTest(unittest.TestCase):
    def run_driver(self, drop=False, duplicate=False):
        import threading

        with response_server(drop, duplicate) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            process = subprocess.Popen(
                [
                    str(ROOT / "src/build/workloads/ranking/DriverNodeRank"),
                    f"--server=127.0.0.1:{server.server_address[1]}",
                    "--threads=1",
                    "--connections=1",
                    "--depth=1",
                    "--qps=20",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                time.sleep(1.5)
                process.send_signal(signal.SIGINT)
                output, _ = process.communicate(timeout=15)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
                server.shutdown()
            self.assertEqual(process.returncode, 0, output)
            line = re.search(r"request_outcomes: (.+)", output)
            self.assertIsNotNone(line, output)
            values = {
                key: float(value)
                for key, value in re.findall(r"(\w+)=([0-9.]+)", line[1])
            }
            self.assertEqual(
                values["sent"],
                values["successful"] + values["failed"] + values["pending"],
            )
            return values, output

    def test_empty_success_is_distinct_from_error(self):
        values, output = self.run_driver()
        self.assertGreater(values["successful"], 5, output)
        self.assertGreater(values["failed"], 5, output)
        self.assertEqual(values["failed"], values["server_errors"])
        qps = float(re.search(r"#: ([0-9.]+) QPS", output)[1])
        self.assertAlmostEqual(
            qps, values["successful"] / values["elapsed_secs"], places=2
        )

    def test_duplicate_responses_do_not_release_capacity_or_inflate_counts(self):
        values, output = self.run_driver(duplicate=True)
        self.assertIn("Ignoring unmatched FeedSim response", output)
        self.assertLessEqual(values["successful"] + values["failed"], values["sent"])
        self.assertEqual(values["failed"], values["server_errors"])
        self.assertEqual(values["transport_errors"], 0)

    def test_connection_drop_counts_as_transport_failure(self):
        values, output = self.run_driver(drop=True)
        self.assertEqual(values["successful"], 0, output)
        self.assertEqual(values["transport_errors"], 1, output)
        self.assertEqual(values["failed"], 1, output)

    def test_dlrm_state_count_uses_cpu_executor_not_io_pool(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / "leaf.log"
            with log.open("w") as output:
                process = subprocess.Popen(
                    [
                        str(ROOT / "src/build/workloads/ranking/LeafNodeRank"),
                        f"--dlrm_model_path={ROOT / 'models/dlrm_small.pt'}",
                        "--io_threads=1",
                        "--cpu_executor_threads=3",
                        "--ranker_threads=1",
                        "--sr_event_base_threads=1",
                        "--noaffinity",
                        "--port=0",
                        "--monitor_port=0",
                    ],
                    stdout=output,
                    stderr=subprocess.STDOUT,
                    env=dict(os.environ, OMP_NUM_THREADS="1"),
                )
                try:
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        if "DLRM states=3 CPU executor threads=3" in log.read_text():
                            break
                        if process.poll() is not None:
                            break
                        time.sleep(0.1)
                    self.assertIn(
                        "DLRM states=3 CPU executor threads=3", log.read_text()
                    )
                finally:
                    process.send_signal(
                        signal.SIGINT
                    ) if process.poll() is None else None
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--feedsim-root", type=Path)
    args, remaining = parser.parse_known_args()
    ROOT = args.feedsim_root
    # The native class decorator is evaluated before CLI parsing.
    NativeTest.__unittest_skip__ = ROOT is None
    unittest.main(argv=[__file__, *remaining])
