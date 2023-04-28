#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import subprocess
import sys
from datetime import datetime


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "packages", "tao_bench")


# Import Benchpress's tao_bench result parser
sys.path.insert(0, str(BENCHPRESS_ROOT))
from benchpress.plugins.parsers.tao_bench import TaoBenchParser


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
        "--interface-name",
        args.interface_name,
        "--port-number",
        str(port_number),
        "--warmup-time",
        str(args.warmup_time),
        "--test-time",
        str(args.test_time),
    ]
    if args.real:
        cmd.append("--real")
    return cmd


def run_server(args):
    # core ranges for each server instance
    n_cores = len(os.sched_getaffinity(0))
    cores_range_1 = f"0-{n_cores // 2 - 1}"
    cores_range_2 = f"{n_cores // 2}-{n_cores - 1}"
    # memory size - split in half for each server
    n_mem = int(args.memsize)
    mem_per_inst = n_mem // 2
    # output log names
    ts = datetime.strftime(datetime.now(), "%y%m%d_%H%M%S")
    logpath1 = os.path.join(BENCHPRESS_ROOT, f"tao-bench-server-1-{ts}.log")
    logpath2 = os.path.join(BENCHPRESS_ROOT, f"tao-bench-server-2-{ts}.log")
    # compose servers: [server_cmds, output_files]
    servers = [
        [
            compose_server_cmd(
                args, cores_range_1, mem_per_inst, args.port_number_inst1
            ),
            open(logpath1, "w"),
        ],
        [
            compose_server_cmd(
                args, cores_range_2, mem_per_inst, args.port_number_inst2
            ),
            open(logpath2, "w"),
        ],
    ]
    # let's spawn servers
    procs = []
    for server in servers:
        print("Spawn server instance: " + " ".join(server[0]))
        p = subprocess.Popen(server[0], stdout=server[1], stderr=server[1])
        procs.append(p)
    # wait for servers to finish
    timeout = args.warmup_time + args.test_time + 240
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
        "successful_instances": 0,
        "role": "server",
        "fast_qps": 0,
        "slow_qps": 0,
        "hit_ratio": 0,
        "total_qps": 0,
        "num_data_points": 0,
    }
    with open(logpath1, "r") as log1:
        parser = TaoBenchParser()
        res = parser.parse(log1, None, 0)
        if "role" in res and res["role"] == "server":
            results.append(res)

    with open(logpath2, "r") as log2:
        parser = TaoBenchParser()
        res = parser.parse(log2, None, 0)
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


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # server-side arguments
    parser.add_argument(
        "--memsize", type=int, required=True, help="memory size in GB, e.g. 64 or 96"
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
        "--interface-name",
        type=str,
        default="eth0",
        help="name of the NIC interface",
    )
    parser.add_argument(
        "--port-number-inst1",
        type=int,
        default=11211,
        help="port number of the server instance 1",
    )
    parser.add_argument(
        "--port-number-inst2",
        type=int,
        default=11212,
        help="port number of the server instance 2",
    )
    parser.add_argument(
        "--warmup-time", type=int, default=2400, help="warmup time in seconds"
    )
    parser.add_argument(
        "--test-time", type=int, default=720, help="test time in seconds"
    )
    parser.add_argument("--real", action="store_true", help="for real")
    # functions
    parser.set_defaults(func=run_server)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
