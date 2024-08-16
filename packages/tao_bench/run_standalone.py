#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import pathlib
import subprocess
import threading
import time

import run_autoscale

BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "packages", "tao_bench")
TAO_BENCH_BM_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")


def init_parser():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--memsize",
        type=int,
        default=run_autoscale.get_system_memsize_gb(),
        help="memory size in GB",
    )
    parser.add_argument(
        "--num-servers",
        type=int,
        default=run_autoscale.get_default_num_servers(),
        help="number of TaoBench server instances",
    )
    parser.add_argument(
        "--fast-threads-ratio",
        type=float,
        default=0.75,
        help="ratio of # fast threads to # logical cores",
    )
    parser.add_argument(
        "--slow-to-fast-ratio",
        type=float,
        default=3,
        help="ratio of # fast threads to # slow threads",
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
        "--clients-per-thread",
        type=int,
        default=0,
        help="number of client connections per thread on the client side. "
        + "This parameater is also used for generating client side commands and instructions. "
        + "Can override the '--conns-per-server-core' parameter.",
    )
    parser.add_argument(
        "--port-number-start",
        type=int,
        default=11211,
        help="starting port number of the servers",
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
    args = parser.parse_args()
    return args


args = init_parser()


def exec_cmd(cmd):
    p = subprocess.Popen(
        cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    stdout, stderr = p.communicate()
    exitcode = p.returncode
    if exitcode != 0:
        print("ERROR: " + str(stderr))
        print("STDOUT: " + str(stdout))
    return stdout, stderr, exitcode


def launch_server():
    cmd = f"{TAO_BENCH_DIR}/run_autoscale.py --real --server-hostname=localhost \
        --clients-per-thread={args.clients_per_thread} --num-servers={args.num_servers} \
        --fast-threads-ratio={args.fast_threads_ratio} --slow-to-fast-ratio={args.slow_to_fast_ratio}\
        --warmup-time={args.warmup_time} --test-time={args.test_time} \
        --port-number-start={args.port_number_start} --bind-cpu={args.bind_cpu} \
        --bind-mem {args.bind_mem} --memsize={args.memsize} --num-clients={args.num_clients} \
        --interface-name=lo"
    stdout, stderr, exitcode = exec_cmd(cmd)
    print(stdout)


t_server = threading.Thread(target=launch_server, args=())
t_server.start()
time.sleep(2)

with open(os.path.join(TAO_BENCH_BM_DIR, "client_instructions.txt"), "r") as f:
    lines = f.readlines()
clients = []
for line in lines:
    if "benchpress_cli" in line:
        clients.append(line.strip())


def launch_client(cmd):
    stdout, stderr, exitcode = exec_cmd(cmd)


t_clients = []
for client in clients:
    cmd = str(BENCHPRESS_ROOT) + client[1:]
    tc = threading.Thread(target=launch_client, args=(cmd,))
    tc.start()
    t_clients.append(tc)

for thread in t_clients:
    thread.join()

t_server.join()
