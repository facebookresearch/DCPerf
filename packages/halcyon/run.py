#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Mapping
from pathlib import Path


PORT = 23459
OWNED_DIRECTORY = "dcperf-halcyon"
OWNERSHIP_MARKER = ".dcperf-halcyon-owned"
MOUNT_RE = re.compile(r"^/mnt/(?:hn|d)(\d+)$")


def _natural_mount_key(path: str) -> tuple[str, int]:
    match = MOUNT_RE.fullmatch(path)
    if match is None:
        return (path, -1)
    return (path[: match.start(1)], int(match.group(1)))


def _run(
    command: list[str], timeout: int | None = None, *, capture: bool = False
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, check=True, capture_output=capture, text=True, timeout=timeout
    )


def _remote(
    ssh_command: str, host: str, command: str, *, check: bool = True
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [*shlex.split(ssh_command), host, command],
        check=check,
        capture_output=True,
        text=True,
    )


def _mounted_block_targets(output: str) -> set[str]:
    targets = set()
    for line in output.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        source, target, options = fields
        if source.startswith("/dev/") and "rw" in options.split(","):
            targets.add(target)
    return targets


def discover_mounts(host: str | None, ssh_command: str) -> list[str]:
    command = ["findmnt", "-rn", "-o", "SOURCE,TARGET,OPTIONS"]
    if host is None:
        output = subprocess.check_output(command, text=True)
        candidates = _mounted_block_targets(output)
        writable = [
            p for p in candidates if MOUNT_RE.fullmatch(p) and os.access(p, os.W_OK)
        ]
    else:
        output = _remote(ssh_command, host, shlex.join(command)).stdout
        candidates = _mounted_block_targets(output)
        writable = []
        for path in candidates:
            if MOUNT_RE.fullmatch(path):
                probe = _remote(
                    ssh_command, host, f"test -w {shlex.quote(path)}", check=False
                )
                if probe.returncode == 0:
                    writable.append(path)
    return sorted(set(writable), key=_natural_mount_key)


def resolve_mounts(value: str | None, host: str | None, ssh_command: str) -> list[str]:
    if not value:
        mounts = discover_mounts(host, ssh_command)
        if not mounts:
            location = host or socket.gethostname()
            raise RuntimeError(
                f"no writable /mnt/hn<N> or /mnt/d<N> mounts found on {location}"
            )
        return mounts

    mounts = [item.strip() for item in value.split(",")]
    mounts = [item for item in mounts if item]
    if not mounts:
        raise ValueError("mount override is empty")
    invalid = [item for item in mounts if not Path(item).is_absolute()]
    if invalid:
        raise ValueError(f"invalid mount override(s): {', '.join(invalid)}")
    command = ["findmnt", "-rn", "-o", "SOURCE,TARGET,OPTIONS"]
    if host is None:
        output = subprocess.check_output(command, text=True)
        eligible = _mounted_block_targets(output)
        invalid = [
            item
            for item in mounts
            if item not in eligible or not os.access(item, os.W_OK)
        ]
    else:
        output = _remote(ssh_command, host, shlex.join(command)).stdout
        eligible = _mounted_block_targets(output)
        invalid = []
        for item in mounts:
            if item not in eligible:
                invalid.append(item)
                continue
            probe = _remote(
                ssh_command, host, f"test -w {shlex.quote(item)}", check=False
            )
            if probe.returncode != 0:
                invalid.append(item)
    if invalid:
        raise ValueError(
            "mount override(s) are not writable block-device mounts: "
            + ", ".join(invalid)
        )
    return sorted(set(mounts), key=_natural_mount_key)


def owned_paths(mounts: list[str]) -> list[str]:
    return [str(Path(mount) / OWNED_DIRECTORY) for mount in mounts]


def prepare_directories(mounts: list[str], host: str | None, ssh_command: str) -> None:
    for directory in owned_paths(mounts):
        marker = str(Path(directory) / OWNERSHIP_MARKER)
        script = (
            f"if test -e {shlex.quote(directory)} && ! test -f {shlex.quote(marker)}; "
            "then echo 'refusing unowned directory' >&2; exit 2; fi; "
            f"mkdir -p {shlex.quote(directory)}; touch {shlex.quote(marker)}"
        )
        if host is None:
            owned = Path(directory)
            marker_path = Path(marker)
            if owned.exists() and not marker_path.is_file():
                raise RuntimeError(f"refusing unowned directory: {owned}")
            owned.mkdir(parents=True, exist_ok=True)
            marker_path.touch()
        else:
            _remote(ssh_command, host, script)


def cleanup_directories(mounts: list[str], host: str | None, ssh_command: str) -> None:
    for directory in owned_paths(mounts):
        marker = str(Path(directory) / OWNERSHIP_MARKER)
        script = (
            f"test -f {shlex.quote(marker)} || "
            "{ echo 'refusing to remove unowned directory' >&2; exit 2; }; "
            f"rm -rf -- {shlex.quote(directory)}"
        )
        if host is None:
            marker_path = Path(marker)
            if not marker_path.is_file():
                raise RuntimeError(f"refusing to remove unowned directory: {directory}")
            shutil.rmtree(directory)
        else:
            _remote(ssh_command, host, script)


def write_standalone_config(path: Path, mounts: list[str]) -> None:
    path.write_text(
        json.dumps(
            {
                "read_ratio": 0.5,
                "write_ratio": 0.5,
                "read_sizes": {"1m": 1.0},
                "write_sizes": {"1m": 1.0},
                "mount_points": owned_paths(mounts),
            },
            indent=2,
        )
        + "\n"
    )


def parse_standalone_output(path: Path) -> dict[str, float]:
    lines = path.read_text().splitlines()
    total = next(
        (fields for line in lines if (fields := line.split()) and fields[0] == "Total"),
        None,
    )
    if total is None or len(total) != 13:
        raise ValueError("standalone output is missing its complete Total row")
    values = [float(value) for value in total[1:]]
    read_qps, write_qps, read_mb, write_mb, read_lat, write_lat = values[:6]
    qps = read_qps + write_qps
    latency = (read_lat * read_qps + write_lat * write_qps) / qps if qps else 0.0
    pool_line = next(
        (line for line in lines if line.startswith("Per-pool CPU (total):")), ""
    )
    pool_values = {
        key: float(value)
        for key, value in re.findall(r"([a-z_]+)=([0-9.]+)", pool_line)
    }
    return {
        "qps": qps,
        "read_qps": read_qps,
        "write_qps": write_qps,
        "throughput_mb_s": read_mb + write_mb,
        "read_throughput_mb_s": read_mb,
        "write_throughput_mb_s": write_mb,
        "latency_ms": latency,
        "read_latency_ms": read_lat,
        "write_latency_ms": write_lat,
        "cpu_util_pct": values[10],
        "disk_util_pct": values[11],
        "network_util_pct": 0.0,
        "reactor_accounted_cpu_ms": pool_values.get("qio_total_cpu_ms", 0.0),
        "reactor_useful_cpu_ratio": 0.0,
    }


def paired_metrics(summary: Mapping[str, object]) -> dict[str, float]:
    def number(key: str) -> float:
        value = summary.get(key)
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise ValueError(f"paired result has invalid or missing metric: {key}")
        return float(value)

    return {
        "qps": number("file_qps"),
        "read_qps": number("file_read_qps"),
        "write_qps": number("file_write_qps"),
        "throughput_mb_s": number("file_xput") / 1e6,
        "read_throughput_mb_s": number("file_read_xput") / 1e6,
        "write_throughput_mb_s": number("file_write_xput") / 1e6,
        "latency_ms": number("file_latency") / 1000.0,
        "read_latency_ms": number("file_read_latency") / 1000.0,
        "write_latency_ms": number("file_write_latency") / 1000.0,
        "cpu_util_pct": number("cpu_util"),
        "disk_util_pct": number("disk_util"),
        "network_util_pct": number("net_util"),
        "reactor_accounted_cpu_ms": (
            number("reactor_useful_busy_ns") + number("reactor_useful_idle_ns")
        )
        / 1e6,
        "reactor_useful_cpu_ratio": number("reactor_useful_cpu_ratio"),
    }


def emit_result(
    mode: str,
    metrics: dict[str, float],
    partial: bool = False,
    returncode: int = 0,
) -> None:
    envelope = {
        "halcyon_result": {
            "schema_version": 1,
            "mode": mode,
            "partial": partial,
            "returncode": returncode,
            "metrics": metrics,
        }
    }
    print(f"HALCYON_RESULT={json.dumps(envelope, sort_keys=True)}")


def emit_paired_result(path: Path, returncode: int) -> None:
    if not path.is_file():
        sys.stderr.write(f"paired result file is missing: {path}\n")
        emit_result("paired_genai", {}, partial=True, returncode=returncode)
        return
    try:
        document = json.loads(path.read_text())
        if not isinstance(document, Mapping):
            raise ValueError("paired result must be a mapping")
        summary = document.get("summary")
        if not isinstance(summary, Mapping):
            raise ValueError("paired result summary must be a mapping")
        metrics = paired_metrics(summary)
    except (json.JSONDecodeError, OSError, ValueError) as error:
        sys.stderr.write(f"invalid paired result file {path}: {error}\n")
        emit_result("paired_genai", {}, partial=True, returncode=returncode)
        return
    emit_result(
        "paired_genai",
        metrics,
        bool(document.get("partial", False)) or returncode != 0,
        returncode,
    )


def wait_for_port(host: str, timeout: int) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, PORT), timeout=1):
                return
        except OSError:
            time.sleep(0.25)
    raise TimeoutError(f"Halcyon daemon on {host}:{PORT} did not become ready")


def run_with_term_timeout(
    command: list[str], timeout: int, env: dict[str, str] | None = None
) -> int:
    process = subprocess.Popen(command, env=env)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        return 124


def install_root() -> Path:
    default = Path(__file__).resolve().parents[2] / "benchmarks" / "halcyon"
    return Path(os.environ.get("HALCYON_ROOT", default))


def python_environment(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    paths = [str(root / "lib" / "python"), str(root / "lib" / "halcyon" / "python")]
    if env.get("PYTHONPATH"):
        paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(paths)
    return env


def paired_config(
    template: str | None,
    sender: str,
    partner: str,
    sender_mounts: list[str],
    partner_mounts: list[str],
) -> dict[str, object]:
    if sender == partner:
        raise ValueError("sender and partner hosts must be different")
    nodes: list[dict[str, object]]
    if template:
        nodes = list(json.loads(Path(template).read_text()).values())
        if len(nodes) != 2:
            raise ValueError("paired config must contain exactly two nodes")
    else:
        node: dict[str, object] = {
            "read_ratio": 0.5,
            "write_ratio": 0.5,
            "read_sizes": {"1m": 1.0},
            "write_sizes": {"1m": 1.0},
            "file_layout": {
                "max_file_size": 8462336,
                "dirs": 100,
                "subdirs": 1,
                "files_per_dir": 100,
            },
            "pair_role": "SendDiskReads",
        }
        nodes = [dict(node), dict(node)]
    nodes[0]["mount_points"] = owned_paths(sender_mounts)
    nodes[0]["target_host"] = partner
    nodes[1]["mount_points"] = owned_paths(partner_mounts)
    nodes[1]["target_host"] = sender
    return {sender: nodes[0], partner: nodes[1]}


def _parse_remote_pid(output: str) -> int:
    prefix = "HALCYON_PID="
    pid_lines = [line for line in output.splitlines() if line.startswith(prefix)]
    if len(pid_lines) != 1 or not pid_lines[0][len(prefix) :].isdigit():
        raise RuntimeError("remote Halcyon daemon did not report a valid PID")
    return int(pid_lines[0][len(prefix) :])


def _stop_remote_daemon_from_pid_file(
    ssh_command: str, partner: str, daemon: Path, pid_file: str
) -> None:
    command = (
        f"pid=$(cat {shlex.quote(pid_file)} 2>/dev/null) || exit 0; "
        'case "$pid" in ""|*[!0-9]*) ;; *) '
        f'if test "$(readlink -f /proc/"$pid"/exe)" = {shlex.quote(str(daemon))}; '
        'then kill -TERM "$pid"; fi ;; esac; '
        f"rm -f {shlex.quote(pid_file)}"
    )
    _remote(ssh_command, partner, command, check=False)


def start_daemons(
    args: argparse.Namespace, root: Path
) -> tuple[subprocess.Popen[bytes], int]:
    daemon = root / "bin" / "HalcyonServiceMain"
    daemon_args = [
        str(daemon),
        f"--port={PORT}",
        f"--qnet_pool_size={args.qnet_threads}",
        f"--qmeta_pool_size={args.qmeta_threads}",
    ]
    local = subprocess.Popen(
        daemon_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    remote_pid = None
    remote_pid_file = f"/tmp/dcperf-halcyon-{os.getpid()}-{time.monotonic_ns()}.pid"
    remote_command = (
        f"nohup {shlex.join(daemon_args)} >/tmp/dcperf-halcyon.log 2>&1 </dev/null "
        f'& pid=$!; printf "%s\\n" "$pid" > {shlex.quote(remote_pid_file)}; '
        'printf "HALCYON_PID=%s\\n" "$pid"'
    )
    try:
        remote_result = _remote(args.ssh_command, args.partner, remote_command)
        remote_pid = _parse_remote_pid(remote_result.stdout)
        _remote(
            args.ssh_command,
            args.partner,
            f"rm -f {shlex.quote(remote_pid_file)}",
            check=False,
        )
        wait_for_port("127.0.0.1", 30)
        wait_for_port(args.partner, 30)
        return local, remote_pid
    except BaseException:
        if remote_pid is None:
            _stop_remote_daemon_from_pid_file(
                args.ssh_command, args.partner, daemon, remote_pid_file
            )
        stop_daemons(local, remote_pid, args.partner, args.ssh_command, root)
        raise


def stop_daemons(
    local: subprocess.Popen[bytes],
    remote_pid: int | None,
    partner: str,
    ssh_command: str,
    root: Path,
) -> None:
    if local.poll() is None:
        local.terminate()
        try:
            local.wait(timeout=15)
        except subprocess.TimeoutExpired:
            local.kill()
            local.wait()
    if remote_pid is None:
        return
    daemon = root / "bin" / "HalcyonServiceMain"
    ownership_check = (
        f'test "$(readlink -f /proc/{remote_pid}/exe)" = {shlex.quote(str(daemon))} '
    )
    owned = _remote(ssh_command, partner, ownership_check, check=False)
    if owned.returncode != 0:
        return
    _remote(ssh_command, partner, f"kill -TERM {remote_pid}", check=False)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        alive = _remote(ssh_command, partner, f"kill -0 {remote_pid}", check=False)
        if alive.returncode != 0:
            return
        time.sleep(0.25)
    still_owned = _remote(ssh_command, partner, ownership_check, check=False)
    if still_owned.returncode == 0:
        _remote(ssh_command, partner, f"kill -KILL {remote_pid}", check=False)


def paired(args: argparse.Namespace, prepare: bool) -> int:
    if not args.partner:
        raise ValueError("--partner is required for paired mode")
    root = install_root()
    sender = args.sender_host or socket.getfqdn()
    sender_mounts = resolve_mounts(args.mounts, None, args.ssh_command)
    partner_mounts = resolve_mounts(args.partner_mounts, args.partner, args.ssh_command)
    prepare_directories(sender_mounts, None, args.ssh_command)
    prepare_directories(partner_mounts, args.partner, args.ssh_command)
    config_data = paired_config(
        args.config, sender, args.partner, sender_mounts, partner_mounts
    )
    with tempfile.TemporaryDirectory(prefix="halcyon-paired-") as tmp:
        config = Path(tmp) / "paired.json"
        config.write_text(json.dumps(config_data, indent=2) + "\n")
        local, remote_pid = start_daemons(args, root)
        try:
            command = [
                str(root / "venv" / "bin" / "python3"),
                str(root / "bin" / "halcyon_client.py"),
                "create" if prepare else "run",
                "--config-file",
                str(config),
                "--threads-per-disk",
                str(args.threads_per_disk),
                "--network-threads",
                str(args.qnet_threads),
                "--metadata-backend",
                args.metadata_backend,
            ]
            if prepare:
                timeout = 24 * 60 * 60
            else:
                command.extend(
                    [
                        "--warmup",
                        str((args.warmup + 59) // 60),
                        "--runtime",
                        str((args.runtime + 59) // 60),
                        "--qflush-threads",
                        str(args.qflush_threads),
                        "--qmeta-threads",
                        str(args.qmeta_threads),
                        "--output-file",
                        args.output,
                    ]
                )
                timeout = args.warmup + args.runtime + 120
            returncode = run_with_term_timeout(
                command, timeout, python_environment(root)
            )
            if not prepare:
                emit_paired_result(Path(args.output), returncode)
            return returncode
        finally:
            stop_daemons(local, remote_pid, args.partner, args.ssh_command, root)


def standalone(args: argparse.Namespace, prepare: bool) -> int:
    mounts = resolve_mounts(args.mounts, None, args.ssh_command)
    prepare_directories(mounts, None, args.ssh_command)
    root = install_root()
    with tempfile.TemporaryDirectory(prefix="halcyon-") as tmp:
        config = Path(tmp) / "standalone.json"
        write_standalone_config(config, mounts)
        command = [
            str(root / "bin" / "halcyon"),
            f"--config_file={config}",
            f"--threads_per_disk={args.threads_per_disk}",
            f"--metadata_backend={args.metadata_backend}",
        ]
        if prepare:
            command.extend(["--create_fileset", "--dirs=100", "--files_per_dir=100"])
        else:
            command.extend(
                [
                    f"--warmup={args.warmup}",
                    f"--runtime={args.runtime}",
                    f"--output_file={args.output}",
                ]
            )
        if prepare:
            return _run(command).returncode
        completed = _run(command)
        emit_result("standalone", parse_standalone_output(Path(args.output)))
        return completed.returncode


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="DCPerf Halcyon harness")
    subparsers = result.add_subparsers(dest="action", required=True)
    for action in ("prepare", "standalone", "paired", "cleanup-filesets"):
        sub = subparsers.add_parser(action)
        sub.add_argument("--mounts", help="comma-separated local mount override")
        sub.add_argument("--partner")
        sub.add_argument("--sender-host")
        sub.add_argument("--partner-mounts")
        sub.add_argument("--ssh-command", default="ssh")
        sub.add_argument("--threads-per-disk", type=int, default=5)
        sub.add_argument("--qnet-threads", type=int, default=16)
        sub.add_argument("--qmeta-threads", type=int, default=0)
        sub.add_argument("--qflush-threads", type=int, default=0)
        sub.add_argument(
            "--metadata-backend", choices=("manifest", "rocksdb"), default="manifest"
        )
        sub.add_argument("--warmup", type=int, default=60, help="seconds")
        sub.add_argument("--runtime", type=int, default=600, help="seconds")
        sub.add_argument("--output", default="halcyon-result.json")
        sub.add_argument("--config")
    return result


def main() -> int:
    args = parser().parse_args()
    if args.action == "standalone":
        return standalone(args, prepare=False)
    if args.action == "prepare" and args.partner is None:
        return standalone(args, prepare=True)
    if args.action == "paired":
        return paired(args, prepare=False)
    if args.action == "prepare":
        return paired(args, prepare=True)
    if args.action == "cleanup-filesets":
        if args.mounts:
            mounts = resolve_mounts(args.mounts, None, args.ssh_command)
        else:
            mounts = discover_mounts(None, args.ssh_command)
        if mounts:
            cleanup_directories(mounts, None, args.ssh_command)
        if args.partner:
            partner_mounts = resolve_mounts(
                args.partner_mounts, args.partner, args.ssh_command
            )
            cleanup_directories(partner_mounts, args.partner, args.ssh_command)
        return 0
    raise AssertionError(f"unhandled action: {args.action}")


if __name__ == "__main__":
    sys.exit(main())
