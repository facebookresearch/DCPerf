#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import json
import os
import pathlib
import re
import shlex
import socket
import subprocess

import sys
import time
from datetime import datetime
from parser import TaoBenchParser

import args_utils

# Add parent directory to path to import diagnosis_utils
sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "common"))
from diagnosis_utils import DiagnosisRecorder


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "packages", "tao_bench")
TAO_BENCH_BM_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")


def find_numa_nodes():
    numa_nodes = {}
    for node_dir in os.listdir("/sys/devices/system/node"):
        if node_dir.startswith("node"):
            node_id = node_dir[4]
            with open(f"/sys/devices/system/node/{node_dir}/cpulist", "r") as f:
                numa_nodes[node_id] = f.read().strip()
    return numa_nodes


NUMA_NODES = find_numa_nodes()


def check_nodes_of_cpu_range(cpu_ranges, numa_nodes):
    def get_start_end(cpu_range):
        start_end = cpu_range.split("-")
        try:
            if len(start_end) < 2:
                return int(start_end[0]), int(start_end[0])
            else:
                return int(start_end[0]), int(start_end[1])
        except ValueError:
            return (-1, -1)

    def is_in_range(node_cpu_ranges, input_cpu_range):
        input_start, input_end = get_start_end(input_cpu_range)
        for node_range in node_cpu_ranges.split(","):
            node_start, node_end = get_start_end(node_range)
            if input_start > node_end or input_end < node_start:
                continue
            return True

    matched_nodes = set()
    for node_id, node_cpu_ranges in numa_nodes.items():
        for cpu_range in cpu_ranges.split(","):
            if is_in_range(node_cpu_ranges, cpu_range):
                matched_nodes.add(node_id)

    return list(matched_nodes)


SERVER_CMD_OPTIONS = []  # To be initialized in init_parser()


def compose_server_cmd(args, cpu_core_range, memsize, port_number):
    server_args = {
        optstr: getattr(args, argkey) for optstr, argkey in SERVER_CMD_OPTIONS
    }
    server_args["--memsize"] = memsize
    server_args["--port-number"] = port_number

    # Add custom thread parameters if specified
    if hasattr(args, "num_fast_threads") and args.num_fast_threads > 0:
        server_args["--num-fast-threads"] = args.num_fast_threads
    if hasattr(args, "num_slow_threads") and args.num_slow_threads > 0:
        server_args["--num-slow-threads"] = args.num_slow_threads

    cmd = [
        "taskset",
        "--cpu-list",
        cpu_core_range,
        os.path.join(TAO_BENCH_DIR, "run.py"),
        "server",
    ]
    for argname, argval in server_args.items():
        if isinstance(argval, bool):
            if argval:
                cmd.append(argname)
        elif argval is not None and argval != 0:
            cmd += [argname, str(argval)]

    if len(NUMA_NODES) > 1 and (args.bind_cpu > 0 or args.bind_mem > 0):
        numa_nodes_belong_to = check_nodes_of_cpu_range(cpu_core_range, NUMA_NODES)
        nodelist = ",".join(numa_nodes_belong_to)
        numactl_cmd = ["numactl"]
        if args.bind_cpu:
            numactl_cmd += ["--cpunodebind", nodelist]
        if args.bind_mem:
            numactl_cmd += ["--membind", nodelist]
        cmd = numactl_cmd + cmd
    if args.real:
        cmd.append("--real")
    print(cmd)
    return cmd


def list2ranges(core_list):
    range_str = f"{core_list[0]}"
    prev = core_list[0]
    for i in core_list[1:]:
        if abs(i - prev) > 1:
            range_str += f"-{prev},{i}"
        prev = i
    range_str += f"-{core_list[-1]}"
    return range_str


def gen_client_instructions(args, to_file=True):
    instruction_text = "Please run the following commands **simultaneously** on all the client machines.\n"
    clients = [""] * args.num_clients
    # If '--client-cores' not specified, assume the client machine has
    # the same number of cores as the server
    if args.client_cores <= 0:
        args.client_cores = len(os.sched_getaffinity(0))
    # Suggest clients_per_thread parameter on the client side
    if args.clients_per_thread > 0:
        clients_per_thread = args.clients_per_thread
    elif args.conns_per_server_core > 0:
        clients_per_thread = (
            args.conns_per_server_core
            * len(os.sched_getaffinity(0))
            // ((args.client_cores - 6) * max(args.num_servers, args.num_clients))
        )
    else:
        clients_per_thread = 0

    if args.server_hostname:
        server_hostname = args.server_hostname
    else:
        server_hostname = socket.gethostname()

    if os.path.exists(os.path.join(BENCHPRESS_ROOT, "benchpress_cli.py")):
        benchpress = "./benchpress_cli.py"
    else:
        benchpress = "./benchpress"

    if args.num_servers > args.num_clients:
        for i in range(args.num_servers):
            c = i % args.num_clients
            client_args = {
                "server_hostname": server_hostname,
                "server_memsize": args.memsize / args.num_servers,
                "warmup_time": args_utils.get_warmup_time(args),
                "test_time": args.test_time,
                "server_port_number": args.port_number_start + i,
            }
            if clients_per_thread > 0:
                client_args["clients_per_thread"] = clients_per_thread
            if args.sanity > 0 and i == 0:
                client_args["sanity"] = args.sanity
            # Set client_id starting from 1
            client_args["client_id"] = i + 1
            if args.client_wait_after_warmup >= 0:
                client_args["wait_after_warmup"] = args.client_wait_after_warmup
            if args.disable_tls != 0:
                client_args["disable_tls"] = 1
            if hasattr(args, "num_client_threads") and args.num_client_threads > 0:
                client_args["num_threads"] = args.num_client_threads
            clients[c] += (
                " ".join(
                    [
                        benchpress,
                        "run",
                        "tao_bench_custom",
                        "-r",
                        "client",
                        "-i",
                        "'" + json.dumps(client_args) + "'",
                    ]
                )
                + "\n"
            )
    else:
        for i in range(args.num_clients):
            s = i % args.num_servers
            client_args = {
                "server_hostname": server_hostname,
                "server_memsize": args.memsize / args.num_servers,
                "warmup_time": args_utils.get_warmup_time(args),
                "test_time": args.test_time,
                "server_port_number": args.port_number_start + s,
            }
            if clients_per_thread > 0:
                client_args["clients_per_thread"] = clients_per_thread
            if args.sanity > 0 and i == 0:
                client_args["sanity"] = args.sanity
            # Set client_id starting from 1
            client_args["client_id"] = i + 1
            if args.client_wait_after_warmup >= 0:
                client_args["wait_after_warmup"] = args.client_wait_after_warmup
            if args.disable_tls != 0:
                client_args["disable_tls"] = 1
            if hasattr(args, "num_client_threads") and args.num_client_threads > 0:
                client_args["num_threads"] = args.num_client_threads
            clients[i] += (
                " ".join(
                    [
                        benchpress,
                        "run",
                        "tao_bench_custom",
                        "-r",
                        "client",
                        "-i",
                        "'" + json.dumps(client_args) + "'",
                    ]
                )
                + "\n"
            )
    for i in range(len(clients)):
        instruction_text += f"Client {i+1}:\n"
        instruction_text += clients[i] + "\n"

    if to_file:
        with open(os.path.join(TAO_BENCH_BM_DIR, "client_instructions.txt"), "w") as f:
            f.write(instruction_text)
    else:
        return instruction_text


def distribute_cores(n_parts):
    core_ranges = []
    # check for SMT
    is_smt_active = False
    try:
        with open("/sys/devices/system/cpu/smt/active", "r") as f:
            smt = f.read().strip()
            if smt == "1":
                is_smt_active = True
    except FileNotFoundError:
        print(
            "Warning: /sys/devices/system/cpu/smt/active not found, "
            + "treating the system as no SMT/hyperthreading."
        )
    # core ranges for each server instance
    n_cores = len(os.sched_getaffinity(0))
    core_list = list(os.sched_getaffinity(0))
    if is_smt_active:
        phy_core_list = core_list[: n_cores // 2]
        smt_core_list = core_list[n_cores // 2 :]
        portion = n_cores // n_parts // 2
        remaining_cores = n_cores - portion * 2 * n_parts
    else:
        phy_core_list = core_list
        portion = n_cores // n_parts
        remaining_cores = n_cores - portion * n_parts
    # Pin each instance to physical cpu core and corresponding vcpu
    core_start_idx = 0
    for i in range(n_parts):
        extra = 1 if remaining_cores > 0 else 0
        cores_to_alloc = phy_core_list[
            core_start_idx : core_start_idx + portion + extra
        ]
        remaining_cores -= extra
        if is_smt_active:
            cores_to_alloc += smt_core_list[
                core_start_idx : core_start_idx + portion + extra
            ]
            remaining_cores -= extra
        core_start_idx += portion + extra
        core_ranges.append(list2ranges(cores_to_alloc))
    return core_ranges


def run_server(args):
    # Create DiagnosisRecorder instance (automatically manages env var for subprocesses)
    recorder = DiagnosisRecorder.get_instance(root_dir=str(BENCHPRESS_ROOT))

    core_ranges = distribute_cores(args.num_servers)
    # memory size - split evenly for each server
    n_mem = float(args.memsize)
    mem_per_inst = n_mem / args.num_servers
    ts = datetime.strftime(datetime.now(), "%y%m%d_%H%M%S")
    # compose servers: [server_cmd, output_file, logpath]
    servers = []
    for i in range(args.num_servers):
        logpath = os.path.join(BENCHPRESS_ROOT, f"tao-bench-server-{i + 1}-{ts}.log")
        servers.append(
            [
                compose_server_cmd(
                    args, core_ranges[i], mem_per_inst, args.port_number_start + i
                ),
                open(logpath, "w"),
                logpath,
            ]
        )
    # generate client side instructions
    if args.real:
        gen_client_instructions(args)
    if args.sanity > 0:
        cmd = "iperf3 -s -1"
        p = subprocess.run(["iperf3", "-s", "-1"], capture_output=True)
        stdout = p.stdout.decode()
        client_ip = stdout.split("Accepted connection from")[1].split(",")[0]
        bandwidth = stdout.split()[-3]
        cmd = f"ping -c 4 {client_ip}"
        p = subprocess.run(shlex.split(cmd), capture_output=True)
        stdout = p.stdout.decode()
        pattern = r"rtt min/avg/max/mdev = \d+\.\d+/(\d+\.\d+)/"
        match = re.search(pattern, stdout)
        if match:
            latency = match.group(1)

    # let's spawn servers
    procs = []
    for server in servers:
        print("Spawn server instance: " + " ".join(server[0]))
        # Use process group to kill all children
        p = subprocess.Popen(
            server[0],
            stdout=server[1],
            stderr=server[1],
            start_new_session=True,  # Create new process group
        )
        procs.append(p)
    # wait for servers to finish - add extra time to make sure
    # post-processing will finish
    if args.poll_interval > 0:
        # Use intelligent process polling instead of fixed timeout
        # First wait for base timeout (warmup + test + timeout_buffer)
        base_timeout = (
            args_utils.get_warmup_time(args) + args.test_time + args.timeout_buffer
        )

        print(f"Waiting {base_timeout}s for processes to complete normally...")
        time.sleep(base_timeout)

        # Poll for additional postprocessing_timeout_buffer time
        # Check if log files are still being written to
        print(
            f"Polling for up to {args.postprocessing_timeout_buffer}s for processes to finish writing output..."
        )
        start_time = time.time()

        # Track file sizes to detect when processes stop writing
        # Dict: logpath -> (last_size, stable_count)
        file_states = {server[2]: (0, 0) for server in servers}
        stable_threshold = (
            3  # Number of consecutive stable checks before considering done
        )

        while time.time() - start_time < args.postprocessing_timeout_buffer:
            all_stable = True

            for server in servers:
                logpath = server[2]
                try:
                    # Flush the file buffer to ensure size is up-to-date
                    server[1].flush()
                    os.fsync(server[1].fileno())

                    current_size = os.path.getsize(logpath)
                    last_size, stable_count = file_states[logpath]

                    if current_size == last_size and current_size != 0:
                        # File size hasn't changed, increment stable count
                        stable_count += 1
                        file_states[logpath] = (current_size, stable_count)

                        if stable_count < stable_threshold:
                            all_stable = False
                    else:
                        # File is still growing or is zero, reset stable count
                        file_states[logpath] = (current_size, 0)
                        all_stable = False

                except (OSError, ValueError):
                    # File might be closed or inaccessible, consider it stable
                    pass

            if all_stable:
                elapsed = time.time() - start_time
                print(f"All log files stable after {elapsed:.2f}s of polling")
                break

            time.sleep(args.poll_interval)
        else:
            # Timeout reached - kill any remaining processes
            total_time = base_timeout + args.postprocessing_timeout_buffer
            print(
                f"Timeout reached after {total_time}s (base: {base_timeout}s + polling: {args.postprocessing_timeout_buffer}s), killing remaining processes"
            )
            for p in procs:
                if p.poll() is None:  # Process still running
                    try:
                        os.killpg(os.getpgid(p.pid), 9)
                    except (ProcessLookupError, PermissionError):
                        pass

        # Ensure all processes are collected and output is flushed
        for p in procs:
            try:
                p.communicate(timeout=1)
            except subprocess.TimeoutExpired:
                pass

        # Explicitly flush all log file handles to ensure data is written to disk
        for server in servers:
            try:
                server[1].flush()  # Flush the file buffer
                os.fsync(server[1].fileno())  # Force OS to write to disk
            except (OSError, ValueError):
                pass  # Handle already closed files or invalid file descriptors

        # Final cleanup to ensure process groups are terminated
        for p in procs:
            try:
                os.killpg(os.getpgid(p.pid), 9)
            except (ProcessLookupError, PermissionError):
                pass  # Process already terminated or we don't have permission
    else:
        # Original behavior with fixed timeout
        timeout = (
            args_utils.get_warmup_time(args)
            + args.test_time
            + args.timeout_buffer
            + args.postprocessing_timeout_buffer
        )

        for p in procs:
            try:
                (out, err) = p.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Kill the entire process group
                try:
                    os.killpg(os.getpgid(p.pid), 9)
                except ProcessLookupError:
                    pass  # Process already terminated
                (out, err) = p.communicate()
            finally:
                # Ensure cleanup even if process completed successfully
                try:
                    os.killpg(os.getpgid(p.pid), 9)
                except (ProcessLookupError, PermissionError):
                    pass  # Process already terminated or we don't have permission
    for server in servers:
        server[1].close()

    # Initialize diagnosis recorder for detailed logging
    recorder = DiagnosisRecorder.get_instance(root_dir=str(BENCHPRESS_ROOT))

    # parse results
    results = []
    overall = {
        "spawned_instances": args.num_servers,
        "successful_instances": 0,
        "role": "server",
        "fast_qps": 0,
        "slow_qps": 0,
        "hit_ratio": 0,
        "total_qps": 0,
        "num_data_points": 0,
    }
    if args.sanity > 0:
        overall["latency(ms)"] = latency
        overall["bandwidth"] = bandwidth

    # Diagnose log files and parsing
    for i in range(args.num_servers):
        logpath = servers[i][2]
        returncode = procs[i].returncode

        # Check if log file exists and get size
        if not os.path.exists(logpath):
            recorder.record_failure(
                benchmark="tao_bench",
                error_type="log_file_missing",
                reason=f"Log file does not exist: {logpath}",
                metadata={"server_index": i, "logpath": logpath},
            )
            print(f"ERROR: Log file does not exist for server {i}: {logpath}")
            continue

        log_size = os.path.getsize(logpath)
        print(
            f"Server {i}: Log file size = {log_size} bytes, return code = {returncode}"
        )

        if log_size == 0:
            recorder.record_failure(
                benchmark="tao_bench",
                error_type="empty_log_file",
                reason=f"Log file is empty: {logpath}",
                metadata={
                    "server_index": i,
                    "logpath": logpath,
                    "returncode": returncode,
                },
            )
            print(f"ERROR: Log file is empty for server {i}: {logpath}")
            continue

        # Parse the log file
        with open(logpath, "r") as log:
            log_content = log.read()
            log.seek(0)  # Reset to beginning for parser

            # Show first few lines for debugging
            preview_lines = log_content.split("\n")[:5]
            print(f"Server {i} log preview (first 5 lines):")
            for line in preview_lines:
                print(f"  {line}")

            parser = TaoBenchParser(f"server_{i}.csv")
            res = parser.parse(log, None, returncode)

            # Diagnose parser results
            print(
                f"Server {i} parsed result: role={res.get('role', 'UNKNOWN')}, "
                f"fast_qps={res.get('fast_qps', 0)}, slow_qps={res.get('slow_qps', 0)}, "
                f"num_data_points={res.get('num_data_points', 0)}"
            )

            if "role" not in res:
                recorder.record_failure(
                    benchmark="tao_bench",
                    error_type="missing_role_in_result",
                    reason=f"Parser result missing 'role' field for server {i}",
                    metadata={
                        "server_index": i,
                        "logpath": logpath,
                        "result": str(res),
                    },
                )
                print(f"ERROR: Parser result missing 'role' field for server {i}")
            elif res["role"] != "server":
                recorder.record_failure(
                    benchmark="tao_bench",
                    error_type="incorrect_role",
                    reason=f"Parser result has incorrect role: {res['role']} (expected 'server')",
                    metadata={
                        "server_index": i,
                        "logpath": logpath,
                        "role": res["role"],
                    },
                )
                print(
                    f"ERROR: Parser result has role '{res['role']}' instead of 'server' for server {i}"
                )
            else:
                # Valid server result
                results.append(res)
                print(f"Server {i}: Successfully parsed and added to results")
    recorder.merge_failure_to_results(
        results_dict=overall,
    )

    for res in results:
        overall["fast_qps"] += res["fast_qps"]
        overall["slow_qps"] += res["slow_qps"]
        overall["total_qps"] += res["total_qps"]
        overall["num_data_points"] += res["num_data_points"]
        overall["hit_ratio"] = (
            overall["hit_ratio"] * overall["successful_instances"] + res["hit_ratio"]
        ) / (overall["successful_instances"] + 1)
        overall["successful_instances"] += 1

    print(json.dumps(overall, indent=4))


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # server-side arguments
    global SERVER_CMD_OPTIONS
    SERVER_CMD_OPTIONS = args_utils.add_common_server_args(parser)
    parser.add_argument(
        "--num-servers",
        type=int,
        default=args_utils.get_default_num_servers(),
        help="number of TaoBench server instances",
    )
    parser.add_argument(
        "--port-number-start",
        type=int,
        default=11211,
        help="starting port number of the servers",
    )
    parser.add_argument(
        "--num-clients",
        type=int,
        default=2,
        help="number of clients to use. This parameter is used for generating client side commands and instructions.",
    )
    parser.add_argument(
        "--server-hostname",
        nargs="?",
        const=socket.gethostname(),
        default=socket.gethostname(),
        type=str,
        help="hostname of the server. This parameter is used for generating client side commands and instructions.",
    )
    parser.add_argument(
        "--clients-per-thread",
        type=int,
        default=0,
        help="number of client connections per thread on the client side. "
        + "This parameater is also used for generating client side commands and instructions. "
        + "Can override the '--conns-per-server-core' parameter.",
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
        "--sanity",
        type=int,
        default=0,
        help="sanity check for the network bandwidth and latency between the server and the client.",
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

    # Add custom thread parameters to SERVER_CMD_OPTIONS
    SERVER_CMD_OPTIONS.append(("--num-fast-threads", "num_fast_threads"))
    SERVER_CMD_OPTIONS.append(("--num-slow-threads", "num_slow_threads"))

    # functions
    parser.set_defaults(func=run_server)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    if args.num_servers == 0:
        args.num_servers = args_utils.get_default_num_servers()
    if args.memsize == 0:
        args.memsize = args_utils.get_system_memsize_gb()
    args.warmup_time = args_utils.get_warmup_time(args)
    args.func(args)
