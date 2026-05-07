#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import pathlib
import re
import resource
import subprocess
import sys
import threading

import args_utils

sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "common"))
from diagnosis_utils import DiagnosisRecorder
from run_autoscale import gen_client_instructions

BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "packages", "tao_bench")
TAO_BENCH_BM_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")


# User setting either server_port_number or port_number_start will result in the same port number between client and server
class SyncPortAction(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        setattr(namespace, self.dest, values)
        if self.dest == "server_port_number":
            namespace.port_number_start = values
        elif self.dest == "port_number_start":
            namespace.server_port_number = values


# User setting either server_port_number or port_number_start will result in the same port number between client and server
class SyncMemsizeAction(argparse.Action):
    def __call__(self, parser, namespace, values, option_string=None):
        setattr(namespace, self.dest, values)
        if self.dest == "server_memsize":
            namespace.memsize = values
        elif self.dest == "memsize":
            namespace.server_memsize = values


SERVER_CMD_OPTIONS = []  # To be initialized in init_parser()


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        conflict_handler="resolve",
    )
    global SERVER_CMD_OPTIONS
    SERVER_CMD_OPTIONS = args_utils.add_common_server_args(parser)
    args_utils.add_common_client_args(parser)

    # Override the default values for server-side arguments
    parser.add_argument("--server-hostname", type=str, help="server hostname")
    parser.add_argument(
        "--server-memsize",
        type=float,
        help="server memory size, e.g. 64, 96",
        action=SyncMemsizeAction,
    )

    for action in parser._actions:
        if action.dest == "server_port_number":
            action.__class__ = SyncPortAction
        elif action.dest == "memsize":
            action.__class__ = SyncMemsizeAction

    parser.add_argument(
        "--num-servers",
        type=int,
        default=args_utils.get_default_num_servers(),
        help="number of TaoBench server instances",
    )
    parser.add_argument(
        "--num-clients",
        type=int,
        default=2,
        help="number of clients to use. This parameter is used for generating client side commands and instructions.",
    )
    parser.add_argument(
        "--port-number-start",
        type=int,
        default=11211,
        help="starting port number of the servers",
        action=SyncPortAction,
    )
    parser.add_argument(
        "--bind-cpu",
        type=int,
        default=1,
        help="explicitly bind TaoBench server instances to dedicated CPU sockets on machines with "
        + "multiple NUMA nodes to minimize cross-socket traffic.",
    )
    parser.add_argument(
        "--bind-mem",
        type=int,
        default=1,
        help="explicitly bind TaoBench server instances to the memory node local to the CPU cores "
        + "on machines with multiple NUMA nodes in order to minimize cross-socket traffic. "
        + "Please set this to 0 if you would like to test hetereogeneous memory systems such as CXL.",
    )
    parser.add_argument(
        "--clients-per-thread",
        type=int,
        default=args_utils.sanitize_clients_per_thread(380),
        help="Number of clients per thread",
    )
    parser.add_argument(
        "--client-cores",
        type=int,
        default=0,
        help="number of logical CPU cores on the client machine. "
        + "If not specified, we will assume the client machine has the same number of cores as this server machine. "
        + "This parameter is used for suggesting clients_per_thread parameter on the client side in accompany with "
        + "'--conns-per-server-core'.",
    )
    parser.add_argument(
        "--conns-per-server-core",
        type=int,
        default=0,
        help="number of client connections per server core to impose. When set to a positive number"
        + "this is used for calculating clients_per_thread parameter to be used on the client side. "
        + "If `--clients-per-thread` is set to a positive number, this parameter will be ignored. ",
    )
    parser.add_argument(
        "--client-wait-after-warmup",
        type=int,
        default=-1,
        help="time in seconds for the client to wait after warmup before starting the test. "
        + " If set to 0 or positive, this will be used in the client instructions.",
    )
    parser.add_argument(
        "--num-client-threads",
        type=int,
        default=0,
        help="number of client threads to use. If not specified, will use default calculation (cores - 6).",
    )
    parser.add_argument(
        "--num-fast-threads",
        type=int,
        default=0,
        help="number of fast threads for the server. If not specified, will use default calculation (cores * fast_threads_ratio).",
    )
    parser.add_argument(
        "--num-slow-threads",
        type=int,
        default=0,
        help="number of slow threads for the server. If not specified, will use default calculation (fast_threads * slow_to_fast_ratio).",
    )
    parser.add_argument(
        "--auto-fix-ports",
        type=int,
        default=0,
        help="automatically reduce clients_per_thread if total connections would exceed "
        + "the ephemeral port range. Set to non-zero to enable.",
    )
    parser.add_argument(
        "--skip-hit-rate-check",
        type=int,
        default=0,
        help="set to 1 to skip the hit rate threshold check when computing "
        + "server QPS. Useful on low core counts where the cache cannot reach "
        + "the 88%% hit rate within a short test.",
    )
    parser.add_argument(
        "--auto-fix-ulimit",
        type=int,
        default=0,
        help="automatically raise the file descriptor soft limit if it is too "
        + "low for the number of connections. Set to non-zero to enable.",
    )
    return parser


def exec_cmd(cmd, output_file=subprocess.PIPE):
    p = subprocess.Popen(
        cmd, shell=True, stdout=output_file, stderr=output_file, text=True, bufsize=1
    )
    stdout, stderr = p.communicate()
    exitcode = p.returncode
    if exitcode != 0:
        print("Command exited with non-zero: " + cmd)
        print("ERROR: " + str(stderr))
        print("STDOUT: " + str(stdout))
    return stdout, stderr, exitcode


def launch_server(port_number_start=11211, bind_cpu=1, bind_mem=1):
    script_args = {
        optstr: getattr(args, argkey) for optstr, argkey in SERVER_CMD_OPTIONS
    }
    script_args["--interface-name"] = "lo"
    script_args["--client-wait-after-warmup"] = 0
    if port_number_start > 0:
        script_args["--port-number-start"] = port_number_start
    script_args["--bind-cpu"] = bind_cpu
    script_args["--bind-mem"] = bind_mem

    # Add custom thread parameters if specified
    if hasattr(args, "num_fast_threads") and args.num_fast_threads > 0:
        script_args["--num-fast-threads"] = args.num_fast_threads
    if hasattr(args, "num_slow_threads") and args.num_slow_threads > 0:
        script_args["--num-slow-threads"] = args.num_slow_threads

    # Add poll_interval if specified
    if hasattr(args, "poll_interval") and args.poll_interval > 0:
        script_args["--poll-interval"] = args.poll_interval

    # Pass through memory file if specified
    if hasattr(args, "memory_file") and args.memory_file:
        script_args["--memory-file"] = args.memory_file

    # Pass through auto-warmup if specified
    if hasattr(args, "auto_warmup") and args.auto_warmup > 0:
        script_args["--auto-warmup"] = args.auto_warmup
    if hasattr(args, "target_hit_ratio") and args.target_hit_ratio != 0.9:
        script_args["--target-hit-ratio"] = args.target_hit_ratio

    # Pass skip-hit-rate-check if specified
    if hasattr(args, "skip_hit_rate_check") and args.skip_hit_rate_check:
        script_args["--skip-hit-rate-check"] = args.skip_hit_rate_check

    cmd = [f"{TAO_BENCH_DIR}/run_autoscale.py --real"]

    for argname, argval in script_args.items():
        if isinstance(argval, bool):
            if argval:
                cmd.append(argname)
        elif argval is not None:
            cmd.extend([argname, str(argval)])

    cmd_str = " ".join(cmd)
    stdout, stderr, exitcode = exec_cmd(cmd_str)
    print(stdout)


def launch_client(cmd, n=1, client_id=0):
    # Use benchpress dry-run to get the real client command
    stdout, stderr, exitcode = exec_cmd(cmd + " --dry-run")
    match = re.search(r"Execution command: (.*)$", stdout)
    if not match:
        print("ERROR: Unable to find the real client command in the output")
        print("STDOUT: " + str(stdout))
        print("STDERR: " + str(stderr))
        exit(1)
    real_cmd = match.group(1)

    # Add client ID flag
    real_cmd += f" --client-id={client_id}"

    with open(f"client_{n}.log", "w") as f:
        _, _, exitcode = exec_cmd(real_cmd, output_file=f)
    return exitcode


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    if args.num_servers == 0:
        args.num_servers = args_utils.get_default_num_servers()
    if args.memsize == 0:
        # Set memory size to 75% of system memory in the standalone mode to avoid OOM,
        # because the clients will also use memory on the same system
        args.memsize = args_utils.get_system_memsize_gb() * 0.75
    args.warmup_time = args_utils.get_warmup_time(args)
    args.server_memsize = args.memsize
    args.server_hostname = "127.0.0.1" if args.ipv4 else "localhost"

    # Initialize DiagnosisRecorder so subprocesses share the same diagnosis file
    recorder = DiagnosisRecorder.get_instance(root_dir=str(BENCHPRESS_ROOT))

    # In standalone mode, all client connections go through loopback and share
    # the same ephemeral port pool. Check if total connections would exceed
    # the available ephemeral ports.
    n_cores = len(os.sched_getaffinity(0))
    if args.num_client_threads > 0:
        threads_per_client = args.num_client_threads
    else:
        threads_per_client = max(1, n_cores - 6, int(n_cores * 0.8))
    if args.clients_per_thread <= 0:
        args.clients_per_thread = args_utils.sanitize_clients_per_thread(380)

    port_low, port_high = "32768", "60999"
    available_ports = 28231  # default: 60999 - 32768
    try:
        with open("/proc/sys/net/ipv4/ip_local_port_range") as f:
            port_low, port_high = f.read().split()
            available_ports = int(port_high) - int(port_low)
    except (OSError, ValueError):
        pass

    max_conns = int(available_ports * 0.8)  # leave 20% margin
    total_conns = args.num_clients * threads_per_client * args.clients_per_thread

    if total_conns > max_conns:
        if args.auto_fix_ports:
            # Calculate the minimum port range needed to support all connections
            # with a 20% safety margin: required_ports = total_conns / 0.8
            required_ports = int(total_conns / 0.8)
            new_port_low = max(1024, 65535 - required_ports)
            new_port_high = 65535
            new_range = f"{new_port_low} {new_port_high}"
            sysctl_cmd = f"sysctl -w net.ipv4.ip_local_port_range='{new_range}'"
            original_range = f"{port_low} {port_high}"

            # Stage 1: Try to widen the ephemeral port range
            port_range_widened = False
            try:
                subprocess.run(
                    ["sysctl", "-w", f"net.ipv4.ip_local_port_range={new_range}"],
                    check=True,
                    capture_output=True,
                )
                port_range_widened = True
                new_available = new_port_high - new_port_low
                recorder.record_auto_fix(
                    benchmark="tao_bench",
                    fix_type="ephemeral_port_range_widened",
                    description=(
                        f"Widened ephemeral port range from '{original_range}' "
                        f"({available_ports} ports) to '{new_range}' "
                        f"({new_available} ports). "
                        f"Total connections needed: {total_conns} "
                        f"(num_clients={args.num_clients} * "
                        f"threads_per_client={threads_per_client} * "
                        f"clients_per_thread={args.clients_per_thread}). "
                        f"Required ports with 20% margin: {required_ports}. "
                        f"Command: {sysctl_cmd}"
                    ),
                    original_value=original_range,
                    fixed_value=new_range,
                    score_impact=(
                        "Positive: widening the port range allows all "
                        f"{total_conns} connections to succeed instead of "
                        f"failing with 'Cannot assign requested address'. "
                        "Without this fix, either connections would be "
                        "reduced (lowering QPS) or connection failures "
                        "would cause errors and lost throughput. With the "
                        "fix, the benchmark runs at full capacity and the "
                        "score reflects the system's true performance."
                    ),
                    metadata={
                        "num_clients": args.num_clients,
                        "threads_per_client": threads_per_client,
                        "clients_per_thread": args.clients_per_thread,
                        "total_connections": total_conns,
                        "original_available_ports": available_ports,
                        "new_available_ports": new_available,
                        "original_port_range": original_range,
                        "new_port_range": new_range,
                        "sysctl_command": sysctl_cmd,
                    },
                )
            except (subprocess.CalledProcessError, FileNotFoundError) as e:
                widen_error = str(e)

            # Stage 2: If widening failed, fall back to reducing connections
            if not port_range_widened:
                original_cpt = args.clients_per_thread
                args.clients_per_thread = max(
                    1, max_conns // (args.num_clients * threads_per_client)
                )
                new_total = (
                    args.num_clients * threads_per_client * args.clients_per_thread
                )
                recorder.record_auto_fix(
                    benchmark="tao_bench",
                    fix_type="ephemeral_port_cap",
                    description=(
                        f"Could not widen ephemeral port range "
                        f"(attempted: {sysctl_cmd}, error: {widen_error}). "
                        f"Falling back to reducing clients_per_thread from "
                        f"{original_cpt} to {args.clients_per_thread}. "
                        f"Original port range: '{original_range}' "
                        f"({available_ports} ports). "
                        f"Needed range: '{new_range}' ({required_ports} "
                        f"ports) to support {total_conns} connections "
                        f"(num_clients={args.num_clients} * "
                        f"threads_per_client={threads_per_client} * "
                        f"clients_per_thread={original_cpt}). "
                        f"Reduced connections: {new_total}."
                    ),
                    original_value=original_cpt,
                    fixed_value=args.clients_per_thread,
                    score_impact=(
                        f"Negative: clients_per_thread was reduced from "
                        f"{original_cpt} to {args.clients_per_thread} "
                        f"({100 - int(args.clients_per_thread / original_cpt * 100)}% "
                        f"fewer connections). This reduces load on the "
                        f"server, resulting in a lower QPS score compared "
                        f"to systems with a wider ephemeral port range. "
                        f"To avoid this penalty, run as root so the port "
                        f"range can be widened: sudo {sysctl_cmd}"
                    ),
                    metadata={
                        "num_clients": args.num_clients,
                        "threads_per_client": threads_per_client,
                        "original_clients_per_thread": original_cpt,
                        "fixed_clients_per_thread": args.clients_per_thread,
                        "original_total_conns": total_conns,
                        "fixed_total_conns": new_total,
                        "original_available_ports": available_ports,
                        "original_port_range": original_range,
                        "attempted_port_range": new_range,
                        "sysctl_command": sysctl_cmd,
                        "widen_error": widen_error,
                    },
                )
                total_conns = new_total
        else:
            error_msg = (
                f"Total client connections ({total_conns}) exceeds the available "
                f"ephemeral port range ({available_ports} ports, "
                f"net.ipv4.ip_local_port_range = {port_low} {port_high}). "
                f"Clients will fail with 'Cannot assign requested address'."
            )
            recorder.record_failure(
                benchmark="tao_bench",
                error_type="ephemeral_port_exhaustion",
                reason=error_msg,
                solutions=[
                    "Expand port range: sudo sysctl -w net.ipv4.ip_local_port_range='1024 65535'",
                    f"Reduce clients_per_thread: --clients-per-thread="
                    f"{max(1, max_conns // (args.num_clients * threads_per_client))}",
                    "Enable auto-fix: --auto-fix-ports=1",
                ],
                metadata={
                    "num_clients": args.num_clients,
                    "threads_per_client": threads_per_client,
                    "clients_per_thread": args.clients_per_thread,
                    "total_connections": total_conns,
                    "available_ports": available_ports,
                    "port_range": f"{port_low}-{port_high}",
                },
            )

    # Check if the file descriptor limit is high enough for all connections.
    # Each connection uses a socket (file descriptor). The server uses -c 180000
    # max connections, and each client opens threads_per_client * clients_per_thread
    # connections. Add overhead for log files, threads, etc.
    fd_overhead = 1000
    required_fds = total_conns + fd_overhead
    soft_limit, hard_limit = resource.getrlimit(resource.RLIMIT_NOFILE)

    if soft_limit < required_fds:
        if args.auto_fix_ulimit:
            new_limit = max(required_fds, 1000000)
            try:
                if hard_limit < new_limit:
                    # Raising hard limit requires root
                    resource.setrlimit(resource.RLIMIT_NOFILE, (new_limit, new_limit))
                else:
                    resource.setrlimit(resource.RLIMIT_NOFILE, (new_limit, hard_limit))
                actual_soft, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
                recorder.record_auto_fix(
                    benchmark="tao_bench",
                    fix_type="file_descriptor_limit",
                    description=(
                        f"Raised file descriptor soft limit from {soft_limit} to "
                        f"{actual_soft} because {total_conns} connections plus "
                        f"overhead require at least {required_fds} file descriptors."
                    ),
                    original_value=soft_limit,
                    fixed_value=actual_soft,
                    score_impact="None — raising the file descriptor limit has no performance impact.",
                    metadata={
                        "original_soft_limit": soft_limit,
                        "original_hard_limit": hard_limit,
                        "new_soft_limit": actual_soft,
                        "required_fds": required_fds,
                        "total_connections": total_conns,
                    },
                )
            except (ValueError, OSError) as e:
                error_msg = (
                    f"File descriptor soft limit ({soft_limit}) is too low for "
                    f"{total_conns} connections (need at least {required_fds}). "
                    f"Auto-fix failed: {e}"
                )
                recorder.record_failure(
                    benchmark="tao_bench",
                    error_type="file_descriptor_limit_low",
                    reason=error_msg,
                    solutions=[
                        f"Run with higher ulimit: sudo bash -c 'ulimit -n {max(required_fds, 100000)} && ./benchpress_cli.py run ...'",
                        "Set system-wide limit in /etc/security/limits.conf",
                    ],
                    metadata={
                        "soft_limit": soft_limit,
                        "hard_limit": hard_limit,
                        "required_fds": required_fds,
                        "total_connections": total_conns,
                        "auto_fix_error": str(e),
                    },
                )
        else:
            error_msg = (
                f"File descriptor soft limit ({soft_limit}) is too low for "
                f"{total_conns} connections (need at least {required_fds}). "
                f"Server and clients may fail with 'Too many open files'."
            )
            recorder.record_failure(
                benchmark="tao_bench",
                error_type="file_descriptor_limit_low",
                reason=error_msg,
                solutions=[
                    "Enable auto-fix: --auto-fix-ulimit=1",
                    f"Run with higher ulimit: sudo bash -c 'ulimit -n {max(required_fds, 100000)} && ./benchpress_cli.py run ...'",
                    "Set system-wide limit in /etc/security/limits.conf",
                ],
                metadata={
                    "soft_limit": soft_limit,
                    "hard_limit": hard_limit,
                    "required_fds": required_fds,
                    "total_connections": total_conns,
                },
            )

    t_server = threading.Thread(
        target=launch_server,
        args=(
            args.port_number_start,
            args.bind_cpu,
            args.bind_mem,
        ),
    )
    t_server.start()

    cmds = gen_client_instructions(args, to_file=False)
    clients = []
    for cmd in cmds.split("\n"):
        if "benchpress" in cmd:
            clients.append(cmd.strip())

    t_clients = []
    for n, client in enumerate(clients):
        cmd = str(BENCHPRESS_ROOT) + client[1:]
        # Set client_id starting from 1 (first client gets ID 1)
        client_id = n + 1
        tc = threading.Thread(
            target=launch_client,
            args=(
                cmd,
                n,
                client_id,
            ),
        )
        tc.start()
        t_clients.append(tc)

    for thread in t_clients:
        thread.join()

    t_server.join()
