#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import socket
import time
from os.path import join as joinpath
from typing import List, Tuple

from config_spark import (
    get_hardware_info,
    get_standalone_cli_args,
    get_standalone_configs,
    init_configs,
)
from utils import exec_cmd, read_environ, run_cmd


key_environ = read_environ()
PROJ_ROOT = key_environ["PROJ_ROOT"]
SPARK_HOME = key_environ["SPARK_HOME"]

HARDWARE_INFO = None
SPARK_CLI_ARGS = None
SPARK_CONFIGS = None

DATA_PATH = joinpath(PROJ_ROOT, "dataset")
CONF_PATH = joinpath(PROJ_ROOT, "settings")
WORK_PATH = joinpath(PROJ_ROOT, "work")
WAREHOUSE_PATH = joinpath(PROJ_ROOT, "warehouse")


def run_spark_sql(
    sql_file: str,
    database: str,
    node0_only: bool = False,
    for_real: bool = False,
) -> None:
    cmd = ["bin/spark-sql"]
    for k, v in SPARK_CLI_ARGS.items():
        cmd.extend([k, v])
    for k, v in SPARK_CONFIGS.items():
        cmd.append("--conf")
        value = '"{}"'.format(" ".join(v)) if type(v) is list else v
        cmd.append(f"{k}={value}")
    cmd.extend(["-f", sql_file])
    if database:
        cmd.extend(["--database", database])
    log_file = sql_file.replace(".sql", ".log")
    env = {}
    if node0_only:
        cmd = ["numactl", "--cpunodebind=0", "--membind=0"] + cmd
    run_cmd(cmd, SPARK_HOME, log_file, env, for_real)
    return None


def write_sql_create_db(args) -> List[str]:
    # print("writing create_db.sql")
    sql_files = []
    filename = joinpath(WORK_PATH, "create_db.sql")
    with open(filename, "wt") as fp:
        fp.write(f"CREATE DATABASE IF NOT EXISTS {args.database};")
    sql_files.append(filename)
    return sql_files


def write_sql_create_tables(args) -> List[str]:
    # print("writing create_tables.sql")
    table_list = []
    dataset_path = joinpath(DATA_PATH, args.database)
    with open(joinpath(dataset_path, "release_info_suite.json"), "rt") as fp:
        query_tests_info = json.load(fp)
        for test_idx, test_info in query_tests_info.items():
            if test_info["status"] != "good":
                continue
            query_dir = joinpath(dataset_path, f"table_{test_idx}")
            table_info_file = joinpath(query_dir, "release_info_test.json")
            with open(table_info_file, "rt") as fp:
                all_lines = fp.readlines()
                for line in all_lines:
                    table_list.append((json.loads(line), query_dir))
    sql_files = []
    filename = joinpath(WORK_PATH, "create_tables.sql")
    with open(filename, "wt") as fp:
        fp.write(f"""USE {args.database};""")
        for table_info, query_dir in table_list:
            table_name = table_info["name"]
            table_src = joinpath(query_dir, table_name)
            partition_keys = ", ".join(table_info["partition_key"])
            fp.write("""\n\nDROP TABLE IF EXISTS tmp_text;""")
            fp.write("""\nCREATE TABLE tmp_text""")
            fp.write("""\nUSING JSON""")
            fp.write(f"""\nOPTIONS (path='{table_src}')""")
            fp.write("""\n;""")
            fp.write(f"""\nDROP TABLE IF EXISTS {table_name};""")
            fp.write(f"""\nCREATE TABLE {table_name}""")
            fp.write("""\nUSING PARQUET""")
            fp.write("""\nOPTIONS (compression='snappy')""")
            fp.write(f"""\nPARTITIONED BY ({partition_keys})""")
            fp.write("""\nAS (SELECT * FROM tmp_text);""")
            fp.write("""\nDROP TABLE IF EXISTS tmp_text;""")
    sql_files.append(filename)
    return sql_files


def list_tests(args) -> None:
    dataset_path = joinpath(DATA_PATH, args.database)
    with open(joinpath(dataset_path, "release_info_suite.json"), "rt") as fp:
        query_tests_info = json.load(fp)
    for test_idx, test_info in query_tests_info.items():
        if test_info["status"] == "good":
            to_print = f"{test_idx}"
            print(to_print)


def write_sql_tests(args) -> List[str]:
    sql_files = []
    dataset_path = joinpath(DATA_PATH, args.database)
    with open(joinpath(dataset_path, "release_info_suite.json"), "rt") as fp:
        query_tests_info = json.load(fp)
    for test_idx, test_info in query_tests_info.items():
        query_dir = joinpath(dataset_path, f"table_{test_idx}")
        if test_info["status"] == "good":
            if len(args.test) == 0 or test_idx in args.test:
                sql_file = test_info["sqlfile"]
                filename = joinpath(WORK_PATH, sql_file)
                # print(f"copying {sql_file}")
                shutil.copy(joinpath(query_dir, sql_file), filename)
                sql_files.append(filename)
    return sql_files


def create(args) -> None:
    setup(args)
    sql_files = write_sql_create_db(args)
    for sql_file in sql_files:
        run_spark_sql(sql_file, database=None, for_real=args.real)
    sql_files = write_sql_create_tables(args)
    for sql_file in sql_files:
        run_spark_sql(sql_file, database=None, for_real=args.real)


def run(args) -> None:
    setup(args)
    sql_files = write_sql_tests(args)
    arch = args.arch if args.arch else HARDWARE_INFO["arch"]
    num_sockets = args.socket if args.socket else HARDWARE_INFO["sockets"]
    (worker_cores, worker_mem) = get_worker_resource(args)
    signature = "/".join(
        [
            arch,
            f"{num_sockets}s",
            f"{args.num_workers}w",
            f"{sum(worker_cores)}c",
            f"{sum(worker_mem)}gb",
            args.numa,
        ]
    )
    with open("results.txt", "at") as fp:  # internal
        if args.real:
            fp.write(f"{args.database} tests:\n")
            fp.write(f"worker-cores : {sum(worker_cores)}\n")
            fp.write(f"worker-memory: {sum(worker_mem)}\n")
        start_time = time.time()
        for _ in range(args.num_iters):
            for sql_file in sql_files:
                cmd = ["echo 1 > /proc/sys/vm/drop_caches"]
                exec_cmd(cmd, args.real)
                test_start_time = time.time()
                run_spark_sql(
                    sql_file,
                    database=args.database,
                    node0_only=(args.numa == "node0_only"),
                    for_real=args.real,
                )
                test_elapsed_time = time.time() - test_start_time
                test_name = os.path.basename(sql_file).split(".")[0]
                print(f"Test {test_name} elapsed time: {test_elapsed_time:.1f} (s)")
                if args.real:
                    fp.write(f"{' '*4}test-{test_name} : {test_elapsed_time:.1f}\n")
                time.sleep(args.interval)
        total_elapsed_time = time.time() - start_time
        print(
            f"Total elapsed time: {total_elapsed_time:.1f} (s) ({args.database} - {signature})"
        )
        if args.real:
            fp.write(f"{args.database} - {signature}: {total_elapsed_time:.1f}\n")


def start(args) -> None:
    setup(args, init=True)
    # driver
    cmd = ["sbin/start-master.sh"]
    log_file = joinpath(WORK_PATH, "start_master.log")
    env = {}
    write_spark_env(args, worker_idx=-1)
    if args.numa == "node0_only":
        cmd = ["numactl", "--cpunodebind=0", "--membind=0"] + cmd
    run_cmd(cmd, SPARK_HOME, log_file, env, args.real)
    # worker(s)
    cmd = [
        "sbin/start-slave-fb.sh",
        SPARK_CLI_ARGS["--master"],
        "-h",
        socket.gethostname(),
    ]
    for wid in range(args.num_workers):
        cmd_prefix = []
        num_sockets = args.socket if args.socket else HARDWARE_INFO["sockets"]
        if args.numa == "none":
            pass
        elif args.numa == "numa_binding" and num_sockets == 2:
            cmd_prefix = ["numactl", f"--cpunodebind={wid}"]
        elif args.numa == "node0_only":
            cmd_prefix = ["numactl", "--cpunodebind=0", "--membind=0"]
        elif args.numa == "cxl_local" and num_sockets == 2:
            cmd_prefix = ["numactl", "--cpunodebind=0", "--membind=0"]
        elif args.numa == "cxl_even" and num_sockets == 2:
            cmd_prefix = ["numactl", "--cpunodebind=0", "--interleave=0,1"]
        elif args.numa == "cxl_binding" and num_sockets == 2:
            cmd_prefix = ["numactl", "--cpunodebind=0", f"--membind={wid}"]
        elif args.numa == "milan_ccx" and num_sockets == 1:
            smt0_start = wid * 6
            smt0_end = smt0_start + 5
            smt1_start = smt0_start + 36
            smt1_end = smt0_end + 36
            cmd_prefix = [
                "numactl",
                f"--physcpubind={smt0_start}-{smt0_end},{smt1_start}-{smt1_end}",
            ]
        else:
            print("WARNING: numa policy not expected; proceed w/o NUMA control")
        log_file = joinpath(WORK_PATH, f"start_slave_{wid}.log")
        full_cmd = cmd_prefix + cmd
        write_spark_env(args, worker_idx=wid)
        if args.num_workers > 1:
            env["SPARK_WORKER_INDEX"] = str(wid)
        if args.numa == "node0_only":
            cmd = ["numactl", "--cpunodebind=0", "--membind=0"] + cmd
        run_cmd(full_cmd, SPARK_HOME, log_file, env, args.real)
    # shuffle server
    cmd = ["sbin/start-shuffle-service.sh"]
    log_file = joinpath(WORK_PATH, "start_shuffle_service.log")
    env = {}
    if args.numa == "node0_only":
        cmd = ["numactl", "--cpunodebind=0", "--membind=0"] + cmd
    run_cmd(cmd, SPARK_HOME, log_file, env, args.real)


def stop(args) -> None:
    # workers
    cmd = ["sbin/stop-slave.sh"]
    log_file = joinpath(WORK_PATH, "stop_slave.log")
    env = {"SPARK_WORKER_INSTANCES": str(args.num_workers)}
    run_cmd(cmd, SPARK_HOME, log_file, env, args.real)
    # shuffle server
    cmd = ["sbin/stop-shuffle-service.sh"]
    log_file = joinpath(WORK_PATH, "stop_shuffle_service.log")
    env = {}
    run_cmd(cmd, SPARK_HOME, log_file, env, args.real)
    # drvier
    cmd = ["sbin/stop-master.sh"]
    log_file = joinpath(WORK_PATH, "stop_master.log")
    env = {}
    run_cmd(cmd, SPARK_HOME, log_file, env, args.real)


def experiment(args) -> None:
    start(args)
    run(args)
    stop(args)


def install(args) -> None:
    start(args)
    create(args)
    stop(args)


def get_worker_resource(args) -> Tuple[List[int]]:
    total_cores = int(SPARK_CONFIGS["spark.cores.max"])
    num_workers = args.num_workers
    if args.worker_cores:
        worker_cores = [int(x) for x in args.worker_cores.split(",")]
    else:
        worker_cores = [int(total_cores / num_workers) for _ in range(num_workers)]
    if args.worker_mem:
        worker_mem = [int(x) for x in args.worker_mem.split(",")]
        if num_workers != len(worker_mem):
            print("Worker mem not specified for all workers")
            exit(1)
    else:
        total_mem = int(HARDWARE_INFO["memory"])
        worker_mem = [int(total_mem / num_workers) for _ in range(num_workers)]
    return (worker_cores, worker_mem)


def write_spark_env(args, worker_idx: int) -> None:
    env_dstfile = joinpath(SPARK_HOME, "conf", "spark-env.sh")
    env_srcfile = joinpath(CONF_PATH, "spark-env-template.sh")
    shuffle_loc = os.path.abspath(args.shuffle_location)
    if not os.path.exists(shuffle_loc):
        os.mkdir(shuffle_loc)
    if worker_idx < 0:
        print(f"set shuffle location at {shuffle_loc}/tmp/")
    (worker_cores, worker_mem) = get_worker_resource(args)
    if worker_idx < 0:
        # driver
        env_cores = sum(worker_cores)
        env_mem = sum(worker_mem)
        env_dst_bak = joinpath(SPARK_HOME, "conf", "spark-env-driver.sh")
    else:
        env_cores = worker_cores[worker_idx]
        env_mem = worker_mem[worker_idx]
        env_dst_bak = joinpath(SPARK_HOME, "conf", f"spark-env-worker{worker_idx}.sh")
    # print(f"writing {env_dst_bak}")
    if worker_idx >= 0:
        print(f"worker cores={env_cores} memory={env_mem}")
    with open(env_dstfile, "wt") as fout:
        with open(env_srcfile, "rt") as fin:
            for line in fin.readlines():
                line = line.replace("__TEMPLATE_SHUFFLE_LOC__", shuffle_loc)
                line = line.replace("__TEMPLATE_WORKER_CORES__", str(env_cores))
                line = line.replace("__TEMPLATE_WORKER_MEMORY__", str(env_mem))
                fout.write(line)
    shutil.copy(env_dstfile, env_dst_bak)


def setup(args, init: bool = False) -> None:
    init_configs(args.aggressive)
    global HARDWARE_INFO, SPARK_CLI_ARGS, SPARK_CONFIGS
    platform = "default-default"
    if args.arch and args.socket:
        platform = f"{args.arch}-{args.socket}s"
    HARDWARE_INFO = get_hardware_info(platform)
    SPARK_CLI_ARGS = get_standalone_cli_args(platform)
    SPARK_CONFIGS = get_standalone_configs(platform)
    # add warehouse directory
    SPARK_CONFIGS["spark.sql.warehouse.dir"] = os.path.abspath(args.database_location)
    if init:
        print(f"set database location at {os.path.abspath(args.database_location)}/")
    # update total core count
    if args.worker_cores:
        worker_cores = [int(x) for x in args.worker_cores.split(",")]
        if args.num_workers != len(worker_cores):
            print("Worker cores not specified for all workers")
            exit(1)
        SPARK_CONFIGS["spark.cores.max"] = str(sum(worker_cores))
    # copy config files
    if init:
        # copy metrics.properties file
        metric_srcfile = joinpath(CONF_PATH, "metrics.properties")
        # print(f"copying {metric_srcfile}")
        if args.real:
            shutil.copy(
                metric_srcfile, joinpath(SPARK_HOME, "conf", "metrics.properties")
            )
        # copy start-slave.sh file
        start_slave_srcfile = joinpath(CONF_PATH, "start-slave-fb.sh")
        # print(f"copying {start_slave_srcfile}")
        if args.real:
            shutil.copy(
                start_slave_srcfile, joinpath(SPARK_HOME, "sbin", "start-slave-fb.sh")
            )


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # sub-command parsers
    sub_parsers = parser.add_subparsers(help="Commands")
    start_parser = sub_parsers.add_parser(
        "start",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="start spark driver/worker",
    )
    stop_parser = sub_parsers.add_parser(
        "stop",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="stop spark driver/worker",
    )
    create_parser = sub_parsers.add_parser(
        "create",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="create database",
    )
    install_parser = sub_parsers.add_parser(
        "install",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="install: start-create-stop ",
    )
    run_parser = sub_parsers.add_parser(
        "run",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="run a test query",
    )
    exp_parser = sub_parsers.add_parser(
        "exp",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="experiment: start-run-stop ",
    )
    list_parser = sub_parsers.add_parser(
        "list",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="list tests",
    )
    # database/suites, tests
    database_choices = os.listdir(DATA_PATH)
    for x in [create_parser, install_parser, run_parser, exp_parser, list_parser]:
        x.add_argument(
            "--database",
            "-d",
            required=True,
            choices=database_choices,
            help="database name; expect same-name folder under dataset/",
        )
    for x in [start_parser, create_parser, install_parser, run_parser, exp_parser]:
        x.add_argument(
            "--database-location",
            "-l",
            type=str,
            default=WAREHOUSE_PATH,
            help="path to warehouse directory",
        )
    for x in [start_parser, install_parser, exp_parser]:
        x.add_argument(
            "--shuffle-location",
            "-k",
            type=str,
            default=PROJ_ROOT,
            help="path to shuffle location; /tmp/ will be appended",
        )
    for x in [run_parser, exp_parser]:
        x.add_argument(
            "--test", "-t", action="append", default=[], help="specify a test ID"
        )
        x.add_argument(
            "--interval", "-i", type=int, default=0, help="sleep time between each test"
        )
        x.add_argument(
            "--num-iters", "-n", type=int, default=1, help="number of iterattiions"
        )
    # platform configs
    for x in [start_parser, create_parser, install_parser, run_parser, exp_parser]:
        x.add_argument(
            "--aggressive", type=int, default=0, help="use aggressive memory configs"
        )
        # internal
        x.add_argument(
            "--arch",
            "-a",
            required=False,
            default=None,
            choices=["bdw", "skl", "cxl_bdw", "cxl_skl"],
            help="set a specific cpu architecture",
        )
        x.add_argument(
            "--socket",
            "-s",
            type=int,
            required=False,
            default=None,
            choices=[1, 2],
            help="number of sockets; always set together w/ arch",
        )
    # number of workers
    for x in [
        start_parser,
        stop_parser,
        create_parser,
        install_parser,
        run_parser,
        exp_parser,
    ]:
        x.add_argument(
            "--num-workers",
            "-w",
            type=int,
            choices=[1, 2, 6],
            default=1,
            help="number of workers per node",
        )
    # customized core count and memory
    for x in [start_parser, run_parser, exp_parser]:
        x.add_argument(
            "--worker-cores",
            "-c",
            default=None,
            help="max_cores for each worker, e.g. 6,4",
        )
        x.add_argument(
            "--worker-mem",
            "-m",
            default=None,
            help="memory for each worker, e.g. 60,30",
        )
    for x in [create_parser, install_parser]:
        x.add_argument("--worker-cores", "-c", default=None, help=argparse.SUPPRESS)
        x.add_argument("--worker-mem", "-m", default=None, help=argparse.SUPPRESS)
    # numa setting
    for x in [start_parser, run_parser, exp_parser]:
        x.add_argument(
            "--numa",
            choices=[
                "none",
                "numa_binding",
                "node0_only",
                "cxl_local",
                "cxl_even",
                "cxl_binding",
                "milan_ccx",
            ],
            default="none",
            help="worker-node binding strategy",
        )
    for x in [install_parser]:
        x.add_argument(
            "--numa",
            choices=[
                "none",
                "numa_binding",
                "node0_only",
                "cxl_local",
                "cxl_even",
                "cxl_binding",
                "milan_ccx",
            ],
            default="none",
            help=argparse.SUPPRESS,
        )
    # as always, dry run or for real
    for x in [
        start_parser,
        stop_parser,
        create_parser,
        install_parser,
        run_parser,
        exp_parser,
    ]:
        x.add_argument("--real", action="store_true", help="for real")
    start_parser.set_defaults(func=start)
    stop_parser.set_defaults(func=stop)
    create_parser.set_defaults(func=create)
    install_parser.set_defaults(func=install)
    run_parser.set_defaults(func=run)
    exp_parser.set_defaults(func=experiment)
    list_parser.set_defaults(func=list_tests)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
