#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import subprocess
import tempfile
from typing import List

import adsim_config


BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[2]
ADSIM_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "adsim")

req_size_dist = [
    21102,
    24046,
    26718,
    29396,
    31428,
    33406,
    35250,
    37347,
    39302,
    40635,
    42162,
    43776,
    45231,
    46550,
    47941,
    49375,
    50490,
    51753,
    52789,
    54042,
    55129,
    56060,
    57210,
    58453,
    59665,
    60732,
    61951,
    63153,
    64345,
    65565,
    66788,
    67846,
    68939,
    70263,
    71538,
    72658,
    74003,
    75320,
    77021,
    78416,
    80017,
    81649,
    83231,
    84890,
    86338,
    87779,
    89549,
    91274,
    92716,
    94312,
    96249,
    98685,
    100195,
    102476,
    104590,
    106565,
    108892,
    112220,
    115426,
    118539,
    121885,
    125940,
    129406,
    133907,
    138907,
    144358,
    150643,
    157272,
    164905,
    173752,
    181697,
    190219,
    199184,
    208782,
    216714,
    226552,
    234955,
    242400,
    250273,
    258056,
    264527,
    274102,
    282639,
    291434,
    302631,
    314295,
    326790,
    338368,
    351672,
    365455,
    378563,
    392808,
    408717,
    428298,
    453281,
    482706,
    517475,
    566604,
    655912,
    2205630,
]


def run_cmd(cmd: List[str], timeout=None, dryrun=False, verbose=False) -> str:
    if verbose or dryrun:
        print(" ".join(cmd))
    if dryrun:
        return None
    if timeout <= 0:
        timeout = None

    proc = subprocess.Popen(cmd)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    return proc.wait()


def run_server(args):
    config_file = args.cfg_file
    timeout = args.timeout

    if config_file == "" or config_file.strip().lower() == "default":
        with tempfile.NamedTemporaryFile(
            mode="w+", prefix="adsim-config-", suffix=".json", delete=False
        ) as cfgfile:
            json.dump(adsim_config.config, cfgfile, indent=2)
            config_file = cfgfile.name

    return run_cmd(
        [
            ADSIM_DIR + "/adsim_server",
            "--config_file",
            config_file,
            "--tlscert",
            ADSIM_DIR + "/configs/certs/example.crt",
            "--tlskey",
            ADSIM_DIR + "/configs/certs/example.key",
            "-stderrthreshold",
            "0",
            "-logtostderr",
            "1",
        ],
        timeout=timeout,
    )


def run_client(args):
    cmd = [
        ADSIM_DIR + "/qps_search.sh",
        "-e",
        ADSIM_DIR + "/treadmill_adsim",
        "-H",
        args.server,
        "-P",
        str(args.port),
        "-R",
        str(args.runtime),
        "-W",
        str(args.workers),
        "-q",
        args.criteria,
        "-l",
        str(args.latency * 1000),
        "-S",
        ",".join([str(sz) for sz in req_size_dist]),
        "-c",
    ]
    return run_cmd(cmd, timeout=args.timeout)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    sub_parsers = parser.add_subparsers(help="Roles")
    server_parser = sub_parsers.add_parser(
        "server",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="run AdSim server",
    )
    client_parser = sub_parsers.add_parser(
        "client",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="run AdSim benchmark driver",
    )
    # server side arguments
    server_parser.add_argument(
        "--cfg-file",
        type=str,
        required=True,
        help="Path to AdSim server config file, "
        + "or 'default' to automatically generate a config file based on adsim-config.py",
    )
    server_parser.add_argument(
        "--timeout", type=int, default=0, help="How long to run the server?"
    )
    # client side arguments
    client_parser.add_argument(
        "--server", type=str, default="::1", help="IP address to the AdSim server"
    )
    client_parser.add_argument(
        "--port", type=int, default=10086, help="Port of the AdSim server"
    )
    client_parser.add_argument(
        "--workers", type=int, default=20, help="Number of workers"
    )
    client_parser.add_argument(
        "--runtime",
        type=int,
        default=120,
        help="Runtime for each latency test in seconds when searching for optimal QPS",
    )
    client_parser.add_argument(
        "--qps-min", type=float, default=1.0, help="Minimum QPS to consider"
    )
    client_parser.add_argument(
        "--qps-max", type=float, default=50.0, help="Maximum QPS to consider"
    )
    client_parser.add_argument(
        "--criteria", type=str, default="P95", help="Target quantile"
    )
    client_parser.add_argument(
        "--latency", type=int, default=989, help="Target latency in ms"
    )
    client_parser.add_argument(
        "--timeout",
        type=int,
        default=1800,
        help="Stop if binary search won't converge within timeout seconds",
    )
    # functions
    server_parser.set_defaults(func=run_server)
    client_parser.set_defaults(func=run_client)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
