#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
from argparse import _HelpAction, ArgumentParser
from typing import List, Sequence, Tuple

MAX_CLIENT_CONN = 32768
MEM_USAGE_FACTOR = 0.75  # to prevent OOM


def sanitize_clients_per_thread(val=380):
    ncores = len(os.sched_getaffinity(0))
    max_clients_per_thread = MAX_CLIENT_CONN // ncores
    return min(val, max_clients_per_thread)


def find_long_option_string(option_strings: Sequence[str]) -> str:
    if len(option_strings) == 1:
        return option_strings[0]
    for option_string in option_strings:
        if option_string.startswith("--"):
            return option_string
    return ""


def get_opt_strings(parser: ArgumentParser) -> List[Tuple[str, str]]:
    res = []
    for action in parser._actions:
        if isinstance(action, _HelpAction):
            continue
        opt_string = find_long_option_string(action.option_strings)
        if opt_string == "":
            continue
        arg_key = action.dest
        res.append((opt_string, arg_key))
    return res


def add_common_server_args(server_parser: ArgumentParser) -> List[Tuple[str, str]]:
    server_parser.add_argument(
        "--memsize", type=float, required=True, help="memory size, e.g. 64 or 96"
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
        "--slow-threads-use-semaphore",
        type=int,
        default=0,
        help="use semaphore to wait for slow requests, set to 0 to turn off",
    )
    server_parser.add_argument(
        "--pin-threads",
        type=int,
        default=0,
        help="pin tao bench threads to dedicated cpu cores, set to nonzero to turn on",
    )
    server_parser.add_argument(
        "--interface-name",
        type=str,
        default="eth0",
        help="name of the NIC interface",
    )
    server_parser.add_argument(
        "--hard-binding",
        action="store_true",
        help="hard bind NIC channels to cores",
    )
    server_parser.add_argument(
        "--stats-interval",
        type=int,
        default=5000,
        help="interval of stats reporting in ms",
    )
    server_parser.add_argument(
        "--timeout-buffer",
        type=int,
        default=120,
        help="extra time the server will wait beyond warmup and test time, "
        + "in seconds, for the clients to start up",
    )
    server_parser.add_argument(
        "--warmup-time", type=int, default=1200, help="warmup time in seconds"
    )
    server_parser.add_argument(
        "--test-time", type=int, default=360, help="test time in seconds"
    )
    server_parser.add_argument(
        "--disable-tls", type=int, default=0, help="set to non-zero to disable TLS"
    )
    server_parser.add_argument(
        "--smart-nanosleep",
        type=int,
        default=0,
        help="randomized nanosleep with exponential backoff",
    )
    server_parser.add_argument("--real", action="store_true", help="for real")

    return get_opt_strings(server_parser)


def add_common_client_args(client_parser: ArgumentParser) -> List[Tuple[str, str]]:
    client_parser.add_argument(
        "--server-hostname", type=str, required=True, help="server hostname"
    )
    client_parser.add_argument(
        "--server-memsize",
        type=float,
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
        "--data-size-min", type=int, default=8191, help="minimum data size"
    )
    client_parser.add_argument(
        "--data-size-max", type=int, default=8193, help="maximum data size"
    )
    client_parser.add_argument(
        "--tunning-factor",
        type=float,
        default=0.807,
        help="tuning factor for key range to get target hit ratio",
    )
    client_parser.add_argument(
        "--clients-per-thread",
        type=int,
        default=sanitize_clients_per_thread(380),
        help="Number of clients per thread",
    )
    client_parser.add_argument(
        "--server-port-number",
        type=int,
        default=11211,
        help="port number of server",
    )
    client_parser.add_argument(
        "--sanity",
        type=int,
        default=0,
        help="sanity check for the network bandwidth and latency between the server and the client.",
    )
    client_parser.add_argument(
        "--wait-after-warmup",
        type=int,
        default=5,
        help="sleep time after warmup in seconds",
    )
    client_parser.add_argument(
        "--warmup-time", type=int, default=1200, help="warmup time in seconds"
    )
    client_parser.add_argument(
        "--test-time", type=int, default=360, help="test time in seconds"
    )
    client_parser.add_argument(
        "--disable-tls", type=int, default=0, help="set to non-zero to disable TLS"
    )
    client_parser.add_argument("--real", action="store_true", help="for real")

    return get_opt_strings(client_parser)
