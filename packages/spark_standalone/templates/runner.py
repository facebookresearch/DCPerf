#!/usr/bin/env python3

import argparse
import os
import pathlib


# SRC_DATASET = "bpc_x1d0_s05"
SRC_DATASET = "beest_perf_common_t93586_s2"

BENCHPRESS_ROOT = pathlib.Path(os.path.abspath(__file__)).parents[3]
SPARK_DIR = os.path.join(BENCHPRESS_ROOT, "benchmarks", "spark_standalone")
WORK_DIR = os.path.join(SPARK_DIR, "work")
DATASET_DIR = os.path.join(SPARK_DIR, "dataset")
CURRENT_DIR = os.getcwd()


def exec_cmd(cmd, for_real=True):
    print(cmd)
    if for_real:
        os.system(cmd)


def download_dataset(args):
    dataset_path = os.path.join(DATASET_DIR, SRC_DATASET)
    if os.path.exists(dataset_path):
        print("Dataset already exists; skip downloading from manifold")
        return
    real_dataset_path = os.path.join(args.dataset_path, SRC_DATASET)
    if not os.path.exists(real_dataset_path):
        manifold_path = (
            f"benchpress_artifacts/tree/spark_standalone/dataset/{SRC_DATASET}/"
        )
        print("Download dataset from manifold")
        exec_cmd(f"manifold getr {manifold_path} {args.dataset_path}", args.real)
    else:
        print("Dataset exists but symlink broken; just fixing symlink")
    exec_cmd(f"rm -f {dataset_path}", args.real)
    exec_cmd(f"ln -s {real_dataset_path} {dataset_path}", args.real)


def install_database(args):
    metadata_dir = os.path.join(SPARK_DIR, "spark-2.4.5-bin-hadoop2.7", "metastore_db")
    database_dir = os.path.join(args.warehouse_dir, f"{SRC_DATASET}.db")
    if os.path.exists(metadata_dir) and os.path.exists(database_dir):
        print("Database already created; directly run test")
        return
    os.chdir(WORK_DIR)
    cmd_list = [
        "../scripts/run_perf_common.py",
        "install",
        "-d",
        SRC_DATASET,
        "-l",
        args.warehouse_dir,
        "-k",
        args.shuffle_dir,
    ]
    if args.aggressive > 0:
        cmd_list.append(f"--aggressive {args.aggressive}")
    cmd_list.append("--real")
    print("Create database from dataset")
    exec_cmd(" ".join(cmd_list), args.real)
    os.chdir(CURRENT_DIR)


def run_test(args):
    os.chdir(WORK_DIR)
    print("Run tests")
    cmd_list = [
        "../scripts/run_perf_common.py",
        "exp",
        "-d",
        SRC_DATASET,
        "-l",
        args.warehouse_dir,
        "-k",
        args.shuffle_dir,
    ]
    if args.aggressive > 0:
        cmd_list.append(f"--aggressive {args.aggressive}")
    cmd_list.append("--real")
    exec_cmd(" ".join(cmd_list), args.real)
    exec_cmd("cat results.txt", args.real)
    os.chdir(CURRENT_DIR)


def setup(args):
    print("Step 1: Export SSDs from remote storage nodes")
    print("\ta. find machines that can provide a total of 3+ SSDs")
    print(
        "\tb. on each of the storage nodes & this machine, install kernel w/ nvme(t)-tcp support and reboot"
    )
    print("\tc. on each of those machines, export SSDs")
    print("\t\tfor a single machine that can provide N (N >= 3) SSDs:")
    print(
        "\t\t> ./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n N -s 1 -p 1 --real"
    )
    print("\t\tfor a machine that can provide one spare SSD (needs 3 of such)")
    print(
        "\t\t> ./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n 1 -s 1 -p 1 --real"
    )
    print(
        "\t\tfor a machine that only has a boot/system drive but "
        + "has a spare partition /dev/nvme0n1pX (needs 3 of such)"
    )

    print(
        "\t\t> ./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py exporter setup -n 1 -s 0 -p X --real"
    )
    print(
        "\td. connect the exported SSDs from this machine (on which you will run the benchmark test)"
    )
    print("\t\tafter each of above command finishes, it will print out a command")
    print("\t\ton this machine, execute the generated command")
    print("\te. now you should see multiple NVMe devices from `lsblk` on this machine")
    print("\tf. create a RAID-0 array and mount it")
    print(
        "\t\ton this machine, suppose you have 3 SSDs imported now, starting from nvme1n1"
    )
    print(
        "\t\t> ./packages/spark_standalone/templates/nvme_tcp/setup_nvmet.py importer mount -n 3 -s 1 --real"
    )
    print("\tf. now you should see remote SSDs mounted at /flash23/")
    print("Step 2: Run Spark standalone benchmark")
    print("\t> ./benchpress_cli.py install spark_standalone_remote")
    print("\t> ./benchpress_cli.py run spark_standalone_remote")


def run(args):
    if "flash23" in args.warehouse_dir:
        if not os.path.exists("/flash23/"):
            print(
                "please run './benchpress_cli.py run spark_standalone_remote_setup' to see how to setup"
            )
            return
    exec_cmd(f"mkdir -p {WORK_DIR}")
    exec_cmd(f"mkdir -p {DATASET_DIR}")
    download_dataset(args)
    install_database(args)
    run_test(args)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # sub-command parsers
    sub_parsers = parser.add_subparsers(help="Commands")
    setup_parser = sub_parsers.add_parser(
        "setup",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="setup remote SSDs via NVMe-over-TCP",
    )
    run_parser = sub_parsers.add_parser(
        "run", formatter_class=argparse.ArgumentDefaultsHelpFormatter, help="run tests"
    )
    # arguments
    run_parser.add_argument(
        "--dataset-path",
        type=str,
        default=SPARK_DIR,
        help="where to download the dataset",
    )
    run_parser.add_argument(
        "--warehouse-dir",
        type=str,
        default=os.path.join(SPARK_DIR, "warehouse"),
        help="where to place the database/warehouse",
    )
    run_parser.add_argument(
        "--shuffle-dir",
        type=str,
        default=SPARK_DIR,
        help="where to point the directory for shuffling & temporary data",
    )
    run_parser.add_argument(
        "--aggressive", type=int, default=0, help="aggressive memory settings"
    )
    run_parser.add_argument("--real", action="store_true", help="for real")
    setup_parser.set_defaults(func=setup)
    run_parser.set_defaults(func=run)
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
