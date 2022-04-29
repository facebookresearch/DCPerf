#!/usr/bin/env python3

import argparse
import multiprocessing
import os
import pathlib
import subprocess
import time
from typing import List


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
TAO_BENCH_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "tao_bench")

MEM_USAGE_FACTOR = 0.75  # not really matter


def get_affinitize_nic_path():
    default_path = "/usr/local/bin/affinitize_nic"
    if os.path.exists(default_path):
        return default_path
    else:
        return os.path.join(TAO_BENCH_DIR, "affinitize/affinitize_nic.py")


def run_cmd(
    cmd: List[str],
    timeout=0,
    for_real=True,
) -> str:
    print(" ".join(cmd))
    if for_real:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if timeout > 0:
            time.sleep(timeout)
            proc.terminate()
        (stdout, _) = proc.communicate()
        return stdout.decode("utf-8")
    else:
        return ""


def run_server(args):
    n_cores = multiprocessing.cpu_count()
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
            "-A",
            "all-nodes",
            "--max-cpus",
            str(n_channels),
            "--xps",
        ]
        run_cmd(cmd)
    except Exception as e:
        print(f"Failed to set affinity: {str(e)}")
    # number of threads for various paths
    n_threads = int(n_cores * args.fast_threads_ratio)
    n_dispatchers = int(n_threads * args.dispatcher_to_fast_ratio)
    n_slow_threads = int(n_threads * args.slow_to_fast_ratio)
    # memory size
    n_mem = int(args.memsize * MEM_USAGE_FACTOR) * 1024
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
        "tao_stats_sleep_ms=5000",
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
        "-I",
        "16m",
        "-o",
        ",".join(extended_options),
    ]
    timeout = args.warmup_time + args.test_time + 120
    stdout = run_cmd(server_cmd, timeout, args.real)
    print(stdout)


def get_client_cmd(args, n_seconds):
    # threads
    if args.num_threads > 0:
        n_threads = args.num_threads
    else:
        n_threads = multiprocessing.cpu_count() - 6
        if n_threads <= 0:
            n_threads = int(multiprocessing.cpu_count() * 0.8)
    # mem size
    n_bytes_per_item = 434  # average from collected distribution
    mem_size_mb = int(args.server_memsize * 1024 * MEM_USAGE_FACTOR)
    n_key_min = 1
    n_keys = int(mem_size_mb * 1024 * 1024 / n_bytes_per_item)
    n_key_max = int(n_keys / args.target_hit_ratio)
    n_key_max = int(n_key_max * args.tunning_factor)
    # command
    s_binary = os.path.join(TAO_BENCH_DIR, "tao_bench_client")
    s_host = args.server_hostname
    client_cmd = [
        s_binary,
        "-s",
        s_host,
        "-p",
        "11211",
        "-P",
        "memcache_binary",
        "--key-pattern=R:R",
        "--distinct-client-seed",
        "--randomize",
        "-R",
        "--hide-histogram",
        "--expiry-range=1800-1802",
        "--data-size-range=8191-8193",
        "--ratio=0:1",
        f"--key-minimum={n_key_min}",
        f"--key-maximum={n_key_max}",
        "-t",
        str(n_threads),
        "--clients=380",
        "--threads-coherence=0",
        "--clients-coherence=3",
        "--key-bytes=220",
        f"--test-time={n_seconds}",
    ]
    return client_cmd


def run_client(args):
    print("warm up phase ...")
    cmd = get_client_cmd(args, n_seconds=args.warmup_time)
    stdout = run_cmd(cmd, timeout=0, for_real=args.real)
    print(stdout)
    time.sleep(5)
    print("execution phase ...")
    cmd = get_client_cmd(args, n_seconds=args.test_time)
    stdout = run_cmd(cmd, timeout=0, for_real=args.real)
    print(stdout)


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
    server_parser.add_argument(
        "--memsize", type=int, required=True, help="memory size, e.g. 64 or 96"
    )
    server_parser.add_argument(
        "--nic-channel-ratio",
        type=float,
        default=0.5,
        help="ratio of # NIC channels to # logical cores",
    )
    server_parser.add_argument(
        "--fast-threads-ratio",
        type=float,
        default=0.75,
        help="ratio of # fast threads to # logical cores",
    )
    server_parser.add_argument(
        "--dispatcher-to-fast-ratio",
        type=float,
        default=0.25,
        help="ratio of # dispatchers to # fast threads",
    )
    server_parser.add_argument(
        "--slow-to-fast-ratio",
        type=float,
        default=3,
        help="ratio of # fast threads to # slow threads",
    )
    server_parser.add_argument(
        "--interface-name",
        type=str,
        default="eth0",
        help="name of the NIC interface",
    )
    # client-side arguments
    client_parser.add_argument(
        "--server-hostname", type=str, required=True, help="server hostname"
    )
    client_parser.add_argument(
        "--server-memsize",
        type=int,
        required=True,
        help="server memory size, e.g. 64, 96",
    )
    client_parser.add_argument(
        "--num-threads",
        type=int,
        default=0,
        help="# threads; default 0 - use (core count - 6)",
    )
    client_parser.add_argument(
        "--target-hit-ratio", type=float, default=0.9, help="target hit ratio"
    )
    client_parser.add_argument(
        "--tunning-factor",
        type=float,
        default=0.807,
        help="tuning factor for key range to get target hit ratio",
    )
    # for both server & client
    for x_parser in [server_parser, client_parser]:
        x_parser.add_argument(
            "--warmup-time", type=int, default=1200, help="warmup time in seconds"
        )
        x_parser.add_argument(
            "--test-time", type=int, default=360, help="test time in seconds"
        )
        x_parser.add_argument("--real", action="store_true", help="for real")
    # functions
    server_parser.set_defaults(func=run_server)
    client_parser.set_defaults(func=run_client)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
