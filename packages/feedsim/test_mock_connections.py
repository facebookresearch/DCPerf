#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under the MIT license in the repository root.

"""Network regression tests against an installed FeedSim build."""

import argparse
import contextlib
import os
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextlib.contextmanager
def single_client_proxy(target, delay=0, accepted_sources=None):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    port = listener.getsockname()[1]
    connections = []
    stop = threading.Event()

    def relay(source, destination):
        while not stop.is_set():
            try:
                data = source.recv(65536)
                if not data:
                    break
                destination.sendall(data)
            except OSError:
                break

    def accept_first():
        try:
            inbound, peer = listener.accept()
            if accepted_sources is not None:
                accepted_sources.append(peer[0])
            listener.close()
            time.sleep(delay)
            outbound = socket.create_connection(("127.0.0.1", target))
            connections.extend((inbound, outbound))
            for source, destination in (
                (inbound, outbound),
                (outbound, inbound),
            ):
                threading.Thread(
                    target=relay,
                    args=(source, destination),
                    daemon=True,
                ).start()
        except OSError:
            pass

    threading.Thread(target=accept_first, daemon=True).start()
    try:
        yield port
    finally:
        stop.set()
        listener.close()
        for connection in connections:
            connection.close()


class ConnectionsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.logs = pathlib.Path(self.tmp.name)

    def probe(self, port, tls=False, *options, bind_ips=None):
        env = dict(os.environ, MOCK_TLS=str(int(tls)), GLOG_logtostderr="1")
        if bind_ips is not None:
            env["MOCK_CONNECT_BIND_IPS"] = bind_ips
        return subprocess.run(
            [
                str(ROOT / "src/build/workloads/ranking/mock_services_probe"),
                f"--port={port}",
                *options,
            ],
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
        )

    @contextlib.contextmanager
    def mock_server(self, tls=False, delay=0):
        port = free_port()
        command = [
            str(ROOT / "src/build/workloads/ranking/mock_services/mock_services"),
            f"--port={port}",
            "--mock_io_threads=1",
            f"--silesia_dir={ROOT / 'silesia'}",
        ]
        if tls:
            command += [
                f"--tls_cert={ROOT / 'certs/example.crt'}",
                f"--tls_key={ROOT / 'certs/example.key'}",
            ]
        with (self.logs / f"mock-{port}.log").open("w") as log:
            process = subprocess.Popen(
                [
                    "bash",
                    "-c",
                    'sleep "$1"; shift; exec "$@"',
                    "mock-service",
                    str(delay),
                    *command,
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                yield port
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    def wait_ready(self, port, tls=False):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            result = self.probe(port, tls)
            if result.returncode == 0:
                return
            time.sleep(0.1)
        self.fail(f"Mock service never became ready: {result.stderr}")

    def test_plain_and_tls_readiness(self):
        for tls in (False, True):
            with self.subTest(tls=tls), self.mock_server(tls) as port:
                self.wait_ready(port, tls)
                wrong_protocol = self.probe(port, not tls)
                self.assertNotEqual(wrong_protocol.returncode, 0)

    def test_tls_waits_for_delayed_handshake(self):
        with self.mock_server(tls=True) as target:
            self.wait_ready(target, tls=True)
            with single_client_proxy(target, delay=0.25) as port:
                start = time.monotonic()
                result = self.probe(port, True, "--host=127.0.0.1", "--timeout_ms=2000")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertGreaterEqual(time.monotonic() - start, 0.25)
                self.assertNotRegex(result.stderr, r"retry \d+/")

    def test_source_ip_partition(self):
        with self.mock_server(tls=True) as target:
            self.wait_ready(target, tls=True)
            sources = []
            with single_client_proxy(target, accepted_sources=sources) as port:
                result = self.probe(
                    port, True, "--host=127.0.0.1", bind_ips="127.0.0.2"
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(sources, ["127.0.0.2"])

    def test_ip_family_mismatch_fails_without_retries(self):
        result = self.probe(1, False, "--host=127.0.0.1", "--retries=5", bind_ips="::1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same IP family", result.stderr)
        self.assertNotRegex(result.stderr, r"retry \d+/")

    def test_tcp_listener_is_not_rpc_ready(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            result = self.probe(listener.getsockname()[1])
            self.assertNotEqual(result.returncode, 0)

    def test_retry_exhaustion_and_backoff_cap(self):
        with socket.socket() as reserved:
            reserved.bind(("127.0.0.1", 0))
            start = time.monotonic()
            result = self.probe(reserved.getsockname()[1], False, "--retries=5")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(
            re.findall(r"retry \d+/5 in (\d+) ms", result.stderr),
            ["100", "200", "400", "800", "1000"],
        )
        self.assertGreaterEqual(time.monotonic() - start, 2.4)

    def test_retries_recover_delayed_service(self):
        with self.mock_server(delay=0.35) as port:
            result = self.probe(port, False, "--retries=5")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(result.stderr, r"retry \d+/5")

    def test_runner_waits_or_fails_before_leaf_start(self):
        for mode, expected in (
            ("exit 7", "died during startup"),
            ("exec sleep 30", "failed RPC readiness"),
        ):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                fixture = pathlib.Path(directory)
                shutil.copy(
                    ROOT / "run-feedsim-multi.sh", fixture / "run-feedsim-multi.sh"
                )
                binaries = fixture / "src/build/workloads/ranking"
                (binaries / "mock_services").mkdir(parents=True)
                (fixture / "silesia").mkdir()
                service = binaries / "mock_services/mock_services"
                service.write_text(f"#!/bin/bash\n{mode}\n")
                service.chmod(0o755)
                (binaries / "mock_services_probe").symlink_to(
                    ROOT / "src/build/workloads/ranking/mock_services_probe"
                )
                leaf_marker = fixture / "leaf-started"
                runner = fixture / "run.sh"
                runner.write_text(f"#!/bin/bash\ntouch '{leaf_marker}'\n")
                runner.chmod(0o755)
                result = subprocess.run(
                    ["bash", str(fixture / "run-feedsim-multi.sh"), "-n", "1"],
                    env=dict(
                        os.environ, FEEDSIM_TLS="0", FEEDSIM_MOCK_READY_TIMEOUT_SECS="1"
                    ),
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(leaf_marker.exists())

    def test_runner_propagates_strict_leaf_failure(self):
        command = [
            "bash",
            str(ROOT / "run-feedsim-multi.sh"),
            "-n",
            "1",
            "--io-threads",
            "1",
            "--dlrm-model=models/dlrm_small.pt",
            "--mock-tls=0",
            "--mock-connect-retries=0",
            "--mock-connect-timeout-ms=100",
            "--mock-connect-failure-policy=abort",
        ]
        if {0, 1}.issubset(os.sched_getaffinity(0)):
            command = ["taskset", "--cpu-list", "0,1", *command]
        result = subprocess.run(
            command,
            env=dict(os.environ, FEEDSIM_TLS="1"),
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("feedsim instance process", result.stderr)

    def test_failure_policies_preserve_or_clear_first_client(self):
        with self.mock_server() as target:
            self.wait_ready(target)
            for policy, expected in (
                ("continue", "1/2 clients ready"),
                ("fallback", "0/2 clients ready"),
                ("abort", "aborting LeafNodeRank"),
            ):
                with self.subTest(policy=policy), single_client_proxy(target) as port:
                    self.check_policy(policy, expected, port)

    def check_policy(self, policy, expected, port):
        logfile = self.logs / f"leaf-{policy}.log"
        command = [
            str(ROOT / "src/build/workloads/ranking/LeafNodeRank"),
            f"--dlrm_model_path={ROOT / 'models/dlrm_small.pt'}",
            f"--rpc_dist_path={ROOT / 'rpc_dist.json'}",
            f"--mock_services_port={port}",
            "--mock_services_host=127.0.0.1",
            "--io_threads=1",
            "--cpu_executor_threads=1",
            "--sr_event_base_threads=2",
            "--ranker_threads=1",
            "--cpu_threads=1",
            "--noaffinity",
            "--mock_connect_retries=0",
            "--mock_connect_timeout_ms=100",
            f"--mock_connect_failure_policy={policy}",
            f"--port={free_port()}",
        ]
        with logfile.open("w") as log:
            process = subprocess.Popen(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                env=dict(os.environ, MOCK_TLS="0", OMP_NUM_THREADS="1"),
            )
            try:
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    contents = logfile.read_text()
                    if expected in contents:
                        break
                    if process.poll() is not None:
                        break
                    time.sleep(0.1)
                self.assertIn(expected, logfile.read_text())
                if policy == "abort":
                    self.assertNotEqual(process.wait(timeout=5), 0)
                elif policy == "continue":
                    self.assertNotIn("Falling back to legacy", logfile.read_text())
            finally:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feedsim-root", type=pathlib.Path, required=True)
    args, remaining = parser.parse_known_args()
    ROOT = args.feedsim_root.resolve()
    unittest.main(argv=[__file__, *remaining])
