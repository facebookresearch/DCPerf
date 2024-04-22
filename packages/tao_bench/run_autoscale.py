#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import socket
import subprocess
import sys
from datetime import datetime


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "packages", "tao_bench")
TAO_BENCH_BM_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")


# Import Benchpress's tao_bench result parser
sys.path.insert(0, str(BENCHPRESS_ROOT))
from benchpress.plugins.parsers.tao_bench import TaoBenchParser


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
        if len(start_end) < 2:
            return int(start_end[0]), int(start_end[0])
        else:
            return int(start_end[0]), int(start_end[1])

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


def compose_server_cmd(args, cpu_core_range, memsize, port_number):
    cmd = [
        "taskset",
        "--cpu-list",
        cpu_core_range,
        os.path.join(TAO_BENCH_DIR, "run.py"),
        "server",
        "--memsize",
        str(memsize),
        "--nic-channel-ratio",
        str(args.nic_channel_ratio),
        "--fast-threads-ratio",
        str(args.fast_threads_ratio),
        "--dispatcher-to-fast-ratio",
        str(args.dispatcher_to_fast_ratio),
        "--slow-to-fast-ratio",
        str(args.slow_to_fast_ratio),
        "--slow-threads-use-semaphore",
        str(args.slow_threads_use_semaphore),
        "--pin-threads",
        str(args.pin_threads),
        "--interface-name",
        args.interface_name,
        "--port-number",
        str(port_number),
        "--warmup-time",
        str(get_warmup_time(args)),
        "--test-time",
        str(args.test_time),
    ]
    if len(NUMA_NODES) > 1:
        numa_nodes_belong_to = check_nodes_of_cpu_range(cpu_core_range, NUMA_NODES)
        nodelist = ",".join(numa_nodes_belong_to)
        cmd = ["numactl", "--cpubind", nodelist, "--membind", nodelist] + cmd
    if args.real:
        cmd.append("--real")
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


def gen_client_instructions(args):
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

    if args.num_servers > args.num_clients:
        for i in range(args.num_servers):
            c = i % args.num_clients
            client_args = {
                "server_hostname": socket.gethostname(),
                "server_memsize": args.memsize // args.num_servers,
                "warmup_time": get_warmup_time(args),
                "test_time": args.test_time,
                "server_port_number": args.port_number_start + i,
            }
            if clients_per_thread > 0:
                client_args["clients_per_thread"] = clients_per_thread
            clients[c] += (
                " ".join(
                    [
                        "./benchpress_cli.py",
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
                "server_hostname": socket.gethostname(),
                "server_memsize": args.memsize // args.num_servers,
                "warmup_time": get_warmup_time(args),
                "test_time": args.test_time,
                "server_port_number": args.port_number_start + s,
            }
            if clients_per_thread > 0:
                client_args["clients_per_thread"] = clients_per_thread
            clients[i] += (
                " ".join(
                    [
                        "./benchpress_cli.py",
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

    with open(os.path.join(TAO_BENCH_BM_DIR, "client_instructions.txt"), "w") as f:
        f.write(instruction_text)


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
    core_ranges = distribute_cores(args.num_servers)
    # memory size - split evenly for each server
    n_mem = int(args.memsize)
    mem_per_inst = n_mem // args.num_servers
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
    # let's spawn servers
    procs = []
    for server in servers:
        print("Spawn server instance: " + " ".join(server[0]))
        p = subprocess.Popen(server[0], stdout=server[1], stderr=server[1])
        procs.append(p)
    # wait for servers to finish
    timeout = get_warmup_time(args) + args.test_time + 240
    for p in procs:
        try:
            (out, err) = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            (out, err) = p.communicate()
    for server in servers:
        server[1].close()
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
    for i in range(args.num_servers):
        logpath = servers[i][2]
        with open(logpath, "r") as log:
            parser = TaoBenchParser(f"server_{i}.csv")
            res = parser.parse(log, None, procs[i].returncode)
            if "role" in res and res["role"] == "server":
                results.append(res)

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


def get_proc_meminfo():
    results = {}
    with open("/proc/meminfo", "r") as f:
        for line in f:
            key, value = line.split(":", maxsplit=1)
            vals = value.strip().split(" ", maxsplit=1)
            numeric = int(vals[0])
            if len(vals) > 1 and vals[1].lower() == "kb":
                numeric *= 1024
            results[key] = numeric
    return results


def get_system_memsize_gb():
    meminfo = get_proc_meminfo()
    return meminfo["MemTotal"] // (1024**3)


def get_default_num_servers(max_cores_per_inst=72):
    ncores = len(os.sched_getaffinity(0))
    return (ncores + max_cores_per_inst - 1) // max_cores_per_inst


def get_warmup_time(args, secs_per_gb=5, min_time=1200):
    if args.warmup_time > 0:
        return args.warmup_time
    else:
        time_to_fill = secs_per_gb * args.memsize
        return max(time_to_fill, min_time)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # server-side arguments
    parser.add_argument(
        "--memsize", type=int, default=get_system_memsize_gb(), help="memory size in GB"
    )
    parser.add_argument(
        "--num-servers",
        type=int,
        default=get_default_num_servers(),
        help="number of TaoBench server instances",
    )
    parser.add_argument(
        "--nic-channel-ratio",
        type=float,
        default=0.5,
        help="ratio of # NIC channels to # logical cores",
    )
    parser.add_argument(
        "--fast-threads-ratio",
        type=float,
        default=0.75,
        help="ratio of # fast threads to # logical cores",
    )
    parser.add_argument(
        "--dispatcher-to-fast-ratio",
        type=float,
        default=0.25,
        help="ratio of # dispatchers to # fast threads",
    )
    parser.add_argument(
        "--slow-to-fast-ratio",
        type=float,
        default=3,
        help="ratio of # fast threads to # slow threads",
    )
    parser.add_argument(
        "--slow-threads-use-semaphore",
        type=int,
        default=0,
        help="use semaphore instead of nanosleep to wait for slow requests, set to 1 to turn on",
    )
    parser.add_argument(
        "--pin-threads",
        type=int,
        default=0,
        help="pin threads to dedicated cores, set to 1 to turn on",
    )
    parser.add_argument(
        "--interface-name",
        type=str,
        default="eth0",
        help="name of the NIC interface",
    )
    parser.add_argument(
        "--port-number-start",
        type=int,
        default=11211,
        help="starting port number of the servers",
    )
    parser.add_argument(
        "--warmup-time",
        type=int,
        default=0,
        help="warmup time in seconds, default is max(5 * memsize, 1200)",
    )
    parser.add_argument(
        "--test-time", type=int, default=720, help="test time in seconds"
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
    parser.add_argument("--real", action="store_true", help="for real")
    # functions
    parser.set_defaults(func=run_server)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    if args.num_servers == 0:
        args.num_servers = get_default_num_servers()
    if args.memsize == 0:
        args.memsize = get_system_memsize_gb()
    args.func(args)
