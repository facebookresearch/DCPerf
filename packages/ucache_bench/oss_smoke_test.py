# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _wait_for_server(port: int, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("server exited before accepting connections")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.25):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"server did not listen on port {port}")


def _metric(
    pattern: str, output: str, name: str, *, integer: bool = True
) -> int | float:
    match = re.search(pattern, output, flags=re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"missing {name} in client output")
    return int(match.group(1)) if integer else float(match.group(1))


def _validate_metrics(output: str) -> None:
    summary = output.rsplit("=== UcacheBench Results ===", 1)[-1]
    warmup = summary.split("BENCHMARK PHASE:", 1)[0]
    benchmark = summary.split("BENCHMARK PHASE:", 1)[-1]

    warmup_operations = int(
        _metric(r"Operations:\s+(\d+)\s+\(", warmup, "warmup operations")
    )
    warmup_successes = int(
        _metric(r"SET Successes:\s+(\d+)", warmup, "warmup successes")
    )
    warmup_errors = int(_metric(r"SET Errors:\s+(\d+)", warmup, "warmup errors"))
    total_operations = int(
        _metric(r"Total Operations:\s+(\d+)", benchmark, "total operations")
    )
    qps = float(_metric(r"QPS:\s+([0-9.]+)", benchmark, "QPS", integer=False))

    get_match = re.search(
        r"GET Operations:\s+(\d+).*?Hits:\s+(\d+).*?Misses:\s+(\d+).*?Errors:\s+(\d+)",
        benchmark,
        flags=re.DOTALL,
    )
    set_match = re.search(
        r"SET Operations:\s+(\d+).*?Successes:\s+(\d+).*?Errors:\s+(\d+)",
        benchmark,
        flags=re.DOTALL,
    )
    if get_match is None or set_match is None:
        raise ValueError("missing GET or SET accounting in client output")
    get_operations, get_hits, get_misses, get_errors = map(int, get_match.groups())
    set_operations, set_successes, set_errors = map(int, set_match.groups())

    latencies = [
        float(_metric(rf"{label}:\s+([0-9.]+)", benchmark, label, integer=False))
        for label in ("P50", "P95", "P99", "P99.9")
    ]

    if warmup_operations <= 0 or warmup_successes <= 0 or warmup_errors != 0:
        raise ValueError(
            "warmup did not complete with positive, error-free SET traffic"
        )
    if total_operations <= 0 or not math.isfinite(qps) or qps <= 0:
        raise ValueError("benchmark reported no real traffic")
    if (
        get_operations <= 0
        or set_operations <= 0
        or get_hits <= 0
        or set_successes <= 0
    ):
        raise ValueError("benchmark did not exercise successful GET and SET traffic")
    if get_errors != 0 or set_errors != 0:
        raise ValueError("benchmark reported protocol errors")
    if get_operations + set_operations != total_operations:
        raise ValueError("GET and SET totals do not match total operations")
    if get_hits + get_misses + get_errors != get_operations:
        raise ValueError("GET accounting is inconsistent")
    if set_successes + set_errors != set_operations:
        raise ValueError("SET accounting is inconsistent")
    if any(not math.isfinite(value) or value < 0 for value in latencies):
        raise ValueError("latency percentiles are invalid")
    if latencies != sorted(latencies):
        raise ValueError("latency percentiles are not monotonic")


def _runner_command(runner: Path, port: int) -> list[str]:
    return [
        sys.executable,
        str(runner),
        "client",
        "--server-host=127.0.0.1",
        f"--server-port={port}",
        "--duration-seconds=2",
        "--warmup-seconds=1",
        "--key-count=64",
        "--value-size-min=64",
        "--value-size-max=64",
        "--get-ratio=0.5",
        "--num-proxies=1",
        "--num-threads=1",
        "--max-inflight=1",
        "--warmup-max-inflight=1",
        "--warmup-adaptive-load=0",
        "--additional-fanout=0",
        "--connection-ramp-seconds=0",
        "--use-same-thread-client=1",
        "--connection-timeout-ms=1000",
        "--send-timeout-ms=1000",
        "--real",
    ]


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run the UcacheBench OSS localhost smoke test"
    )
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True)
    args = parser.parse_args()

    server_path = args.server.resolve(strict=True)
    client_path = args.client.resolve(strict=True)
    runner_path = args.runner.resolve(strict=True)

    port = _free_port()
    server_command = [
        str(server_path),
        f"--port={port}",
        "--memory_mb=256",
        "--hash_power=20",
        "--rpc_io_threads=1",
        "--rpc_num_acceptor_threads=1",
        "--rpc_num_cpu_worker_threads=1",
        "--stats_interval_seconds=0",
    ]
    server = subprocess.Popen(
        server_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    with tempfile.TemporaryDirectory() as tmp:
        package_dir = Path(tmp)
        (package_dir / "server").mkdir()
        (package_dir / "client").mkdir()
        (package_dir / "server" / "ucachebench_server").symlink_to(server_path)
        (package_dir / "client" / "ucachebench_client").symlink_to(client_path)
        env = os.environ.copy()
        env["UCACHE_BENCH_DIR"] = str(package_dir)
        env["PYTHONIOENCODING"] = "utf-8"

        try:
            _wait_for_server(port, server)
            positive = subprocess.run(
                _runner_command(runner_path, port),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=45,
            )
            print(positive.stdout)
            if positive.returncode != 0:
                raise RuntimeError(f"positive client run exited {positive.returncode}")
            _validate_metrics(positive.stdout)

            closed_port = _free_port()
            negative = subprocess.run(
                _runner_command(runner_path, closed_port),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=45,
            )
            if negative.returncode == 0:
                print(negative.stdout)
                raise RuntimeError("closed-port client run returned success")
        finally:
            if server.poll() is None:
                server.terminate()
            try:
                server_output, _ = server.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                server.kill()
                server_output, _ = server.communicate()
                print(server_output)
                raise RuntimeError("server did not stop after SIGTERM")
            print(server_output)

    if server.returncode != 0:
        raise RuntimeError(f"server exited {server.returncode}")


if __name__ == "__main__":
    main()
