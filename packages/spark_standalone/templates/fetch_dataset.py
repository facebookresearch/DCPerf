#!/usr/bin/env python3

import argparse
import json
import os
import time


def run_cmd(cmd, for_real):
    cmd_str = " ".join(cmd)
    print(cmd_str)
    if for_real:
        os.system(cmd_str)


def create_dir(dst_path, dirname, for_real):
    if dirname == ".":
        return
    dst_dirname = os.path.join(dst_path, dirname)
    cmd = [f"mkdir -p {os.path.join(dst_path, dst_dirname)}"]
    run_cmd(cmd, for_real)


def fetch_file(dst_path, filename, for_real):
    if filename.startswith(".git"):
        return
    manifold_dataset = "benchpress_artifacts/tree/spark_standalone/dataset/bpc_x1d0_s05"
    manifold_file = os.path.join(manifold_dataset, filename)
    dst_filename = os.path.join(dst_path, filename)
    cmd = ["manifold", "get", manifold_file, dst_filename]
    run_cmd(cmd, for_real)


def fetch_dataset(dst_path, file_list, for_real):
    with open(file_list, "rt") as fp:
        metadata = json.load(fp)
    dir_list = metadata["dirs"]
    file_list = metadata["files"]
    for dirname in dir_list:
        create_dir(dst_path, dirname, for_real)
    for filename in file_list:
        fetch_file(dst_path, filename, for_real)
        time.sleep(0.1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # arguments
    for x in [parser]:
        x.add_argument("--dst-path", "-d", type=str, required=True, help="target path")
        x.add_argument(
            "--file-list", "-f", type=str, required=True, help="file list json file"
        )
        x.add_argument("--real", action="store_true", help="for real")

    args = parser.parse_args()
    fetch_dataset(args.dst_path, args.file_list, args.real)
