#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import os
import pathlib
import shlex
import subprocess
import threading
import time
from typing import List

import args_utils


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")
SERVER_PROFILING_DELAY = 120


def get_affinitize_nic_path():
    default_path = "/usr/local/bin/affinitize_nic"
    if os.path.exists(default_path):
        return default_path
    else:
        return os.path.join(TAO_BENCH_DIR, "affinitize/affinitize_nic.py")


def run_cmd(
    cmd: List[str],
    timeout=None,
    for_real=True,
) -> str:
    print(" ".join(cmd))
    if for_real:
        proc = subprocess.Popen(
            cmd,
            stderr=subprocess.STDOUT,
        )
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait()


def profile_server():
    # check if an existing profile data already exists
    if os.path.exists("perf.data"):
        return
    p_prof = subprocess.run(
        ["perf", "record", "-a", "-g", "-o", "perf.data", "--", "sleep", "5"]
    )
    return p_prof


def affinitize_nic(args):
    n_cores = len(os.sched_getaffinity(0))
    n_channels = int(n_cores * args.nic_channel_ratio)
    # set # channels
    try:
        cmd = ["ethtool", "-L", args.interface_name, "combined", str(n_channels)]
        run_cmd(cmd)
    except Exception as e:
        print(f"Failed to set channels to {n_channels}: {str(e)}")
    # set affinity
    try:
        cmd = [
            get_affinitize_nic_path(),
            "-f",
            "-a",
            "--xps",
        ]
        if args.hard_binding:
            cmd += [
                "--cpu",
                " ".join(str(x) for x in range(n_channels)),
            ]
        else:
            cmd += [
                "-A",
                "all-nodes",
                "--max-cpus",
                str(n_channels),
            ]
        run_cmd(cmd)
    except Exception as e:
        print(f"Failed to set affinity: {str(e)}")


def run_server(args):
    n_cores = len(os.sched_getaffinity(0))
    n_channels = int(n_cores * args.nic_channel_ratio)
    if args.interface_name != "lo":
        affinitize_nic(args)
    # number of threads for various paths
    n_threads = max(int(n_cores * args.fast_threads_ratio), 1)
    n_dispatchers = max(int(n_threads * args.dispatcher_to_fast_ratio), 1)
    n_slow_threads = max(int(n_threads * args.slow_to_fast_ratio), 1)
    # memory size
    n_mem = int(args.memsize * 1024 * args_utils.MEM_USAGE_FACTOR)
    # port number
    if args.port_number > 0:
        port_num = args.port_number
    else:
        port_num = 11211
    print(
        f"Use {n_channels} NIC channels, {n_threads} fast threads and {n_mem} MB cache memory"
    )
    s_binary = os.path.join(TAO_BENCH_DIR, "tao_bench_server")
    extended_options = [
        "lru_crawler",
        f"tao_it_gen_file={os.path.join(TAO_BENCH_DIR, 'leader_sizes.json')}",
        "tao_max_item_size=65536",
        "tao_gen_payload=0",
        f"tao_slow_dispatchers={n_dispatchers}",
        f"tao_num_slow_threads={n_slow_threads}",
        "tao_max_slow_reqs=1024",
        "tao_worker_sleep_ns=100",
        "tao_dispatcher_sleep_ns=100",
        "tao_slow_sleep_ns=100",
        "tao_slow_path_sleep_us=0",
        "tao_compress_items=1",
        f"tao_stats_sleep_ms={args.stats_interval}",
        f"tao_slow_use_semaphore={args.slow_threads_use_semaphore}",
        f"tao_pin_threads={args.pin_threads}",
        f"tao_smart_nanosleep={args.smart_nanosleep}",
    ]
    if not args.disable_tls:
        extended_options += [
            f"ssl_chain_cert={os.path.join(TAO_BENCH_DIR, 'certs/example.crt')}",
            f"ssl_key={os.path.join(TAO_BENCH_DIR, 'certs/example.key')}",
        ]
    server_cmd = [
        s_binary,
        "-c",
        "180000",
        "-u",
        "nobody",
        "-m",
        str(n_mem),
        "-t",
        str(n_threads),
        "-B",
        "binary",
        "-p",
        str(port_num),
        "-I",
        "16m",
    ]
    if not args.disable_tls:
        server_cmd.append("-Z")
    server_cmd += [
        "-o",
        ",".join(extended_options),
    ]
    if "DCPERF_PERF_RECORD" in os.environ and os.environ["DCPERF_PERF_RECORD"] == "1":
        profiler_wait_time = (
            args.warmup_time + args.timeout_buffer + SERVER_PROFILING_DELAY
        )
        t_prof = threading.Timer(profiler_wait_time, profile_server)
        t_prof.start()

    timeout = args.warmup_time + args.test_time + args.timeout_buffer
    run_cmd(server_cmd, timeout, args.real)

    if "DCPERF_PERF_RECORD" in os.environ and os.environ["DCPERF_PERF_RECORD"] == "1":
        t_prof.cancel()


def get_client_cmd(args, n_seconds):
    # threads
    if args.num_threads > 0:
        n_threads = args.num_threads
    else:
        n_threads = len(os.sched_getaffinity(0)) - 6
        if n_threads <= 0:
            n_threads = int(len(os.sched_getaffinity(0)) * 0.8)
    # clients
    if args.clients_per_thread > 0:
        n_clients = args_utils.sanitize_clients_per_thread(args.clients_per_thread)
    else:
        n_clients = args_utils.sanitize_clients_per_thread(380)
    # server port number
    if args.server_port_number > 0:
        server_port_num = args.server_port_number
    else:
        server_port_num = 11211

    # mem size
    n_bytes_per_item = 434  # average from collected distribution
    mem_size_mb = int(args.server_memsize * 1024 * args_utils.MEM_USAGE_FACTOR)
    n_key_min = 1
    n_keys = int(mem_size_mb * 1024 * 1024 / n_bytes_per_item)
    n_key_max = int(n_keys / args.target_hit_ratio)
    n_key_max = int(n_key_max * args.tunning_factor)
    # command
    s_binary = os.path.join(TAO_BENCH_DIR, "tao_bench_client")
    s_host = args.server_hostname
    s_cert = os.path.join(TAO_BENCH_DIR, "./certs/example.crt")
    s_key = os.path.join(TAO_BENCH_DIR, "./certs/example.key")
    client_cmd = [
        s_binary,
        "-s",
        s_host,
        "-p",
        str(server_port_num),
        "-P",
        "memcache_binary",
        "--key-pattern=R:R",
        "--distinct-client-seed",
        "--randomize",
        "-R",
        "--hide-histogram",
        "--expiry-range=1800-1802",
        f"--data-size-range={args.data_size_min}-{args.data_size_max}",
        "--ratio=0:1",
        f"--key-minimum={n_key_min}",
        f"--key-maximum={n_key_max}",
        "-t",
        str(n_threads),
        f"--clients={n_clients}",
        "--threads-coherence=0",
        "--clients-coherence=3",
        "--key-bytes=220",
        f"--test-time={n_seconds}",
    ]
    if not args.disable_tls:
        client_cmd += [
            f"--cert={s_cert}",
            f"--key={s_key}",
            "--tls",
            "--tls-skip-verify",
        ]
    return client_cmd


def run_client(args):
    if args.sanity > 0:
        cmd = f"iperf3 -c {args.server_hostname} -P4"
        subprocess.run(shlex.split(cmd))

    print("warm up phase ...")
    cmd = get_client_cmd(args, n_seconds=args.warmup_time)
    run_cmd(cmd, timeout=args.warmup_time + 30, for_real=args.real)
    if args.real and args.wait_after_warmup > 0:
        time.sleep(args.wait_after_warmup)
    print("execution phase ...")
    cmd = get_client_cmd(args, n_seconds=args.test_time)
    run_cmd(cmd, timeout=args.test_time + 30, for_real=args.real)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # sub-command parsers
    sub_parsers = parser.add_subparsers(help="Commands")
    server_parser = sub_parsers.add_parser(
        "server",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="run server",
    )
    client_parser = sub_parsers.add_parser(
        "client",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="run client",
    )
    # server-side arguments
    args_utils.add_common_server_args(server_parser)
    server_parser.add_argument(
        "--port-number",
        type=int,
        default=11211,
        help="port number of server",
    )

    # client-side arguments
    args_utils.add_common_client_args(client_parser)

    # functions
    server_parser.set_defaults(func=run_server)
    client_parser.set_defaults(func=run_client)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
