#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import logging
import os
import socket
import time

from utils import exec_cmd, InfoLogFormat, is_distro_like, run_cmd, setup_logger


logger = setup_logger(__name__, logging.INFO, InfoLogFormat)


def build_nvme_cli(real):
    if is_distro_like("centos"):
        exec_cmd("dnf install -y libuuid-devel", real)
    elif is_distro_like("ubuntu"):
        exec_cmd("apt install -y libuuid1", real)
        exec_cmd("apt install -y uuid-dev", real)
    ROOT_PATH = os.getcwd()
    cli_util_path = os.path.join(ROOT_PATH, "nvme-cli")
    if not os.path.exists(cli_util_path):
        exec_cmd("git clone https://github.com/pkuwangh/nvme-cli", for_real=True)
    logger.info(f"chdir to {cli_util_path}")
    os.chdir(cli_util_path)
    exec_cmd("git checkout 274a49759c8cbdd991253455c64136e0ea73cb6b", for_real=True)
    exec_cmd("make clean", real)
    exec_cmd("make && make install", real)
    logger.info(f"chdir to {ROOT_PATH}")
    os.chdir(ROOT_PATH)
    exec_cmd("nvme list", real)


def build_nvmetcli(real):
    if is_distro_like("centos"):
        exec_cmd("dnf install -y python3-configshell", real)
    elif is_distro_like("ubuntu"):
        exec_cmd("apt install -y python3-configshell-fb", real)
    ROOT_PATH = os.getcwd()
    cli_util_path = os.path.join(ROOT_PATH, "nvmetcli")
    if not os.path.exists(cli_util_path):
        exec_cmd(
            "git clone https://github.com/pkuwangh/nvmetcli.git -b for_spark",
            for_real=True,
        )

    logger.info(f"chdir to {cli_util_path}")
    os.chdir(cli_util_path)
    exec_cmd("git checkout 7655e1020bfa15f89aa4df125cf3772c23c86396", for_real=True)
    exec_cmd("./setup.py install", real)
    logger.info(f"chdir to {ROOT_PATH}")
    os.chdir(ROOT_PATH)
    exec_cmd("nvmetcli ls", real)


def create_partitions(args):
    for i in range(args.num_devices):
        idx = args.start_device + i
        device_name = f"/dev/nvme{idx}n1"
        logger.info(f"About to use fdisk to create a new partition on {device_name}")
        logger.info(f"You picked (and should use) partition index {args.partition_idx}")
        exec_cmd(f"fdisk {device_name}", args.real)
    exec_cmd("lsblk", args.real)


def export_devices(args, ip_addr):
    for i in range(args.num_devices):
        idx = args.start_device + i
        target_name = f"{args.target_name}-{idx}"
        target_path = f"/sys/kernel/config/nvmet/subsystems/{target_name}"
        exec_cmd(f"mkdir -p {target_path}", args.real)
        exec_cmd(
            f"echo 1 | tee -a {target_path}/attr_allow_any_host > /dev/null", args.real
        )
        ns_path = f"{target_path}/namespaces/1"
        exec_cmd(f"mkdir -p {ns_path}", args.real)
        target_device = f"/dev/nvme{idx}n1p{args.partition_idx}"
        exec_cmd(
            f"echo -n {target_device} | tee -a {ns_path}/device_path > /dev/null",
            args.real,
        )
        exec_cmd(f"echo 1 | tee -a {ns_path}/enable > /dev/null", args.real)
    port_path = "/sys/kernel/config/nvmet/ports/1"
    exec_cmd(f"mkdir -p {port_path}", args.real)
    exec_cmd(f"echo {ip_addr} | tee -a {port_path}/addr_traddr > /dev/null", args.real)
    ip_format = "ipv4" if args.ipv4 else "ipv6"
    exec_cmd(
        f"echo {ip_format} | tee -a {port_path}/addr_adrfam > /dev/null", args.real
    )
    exec_cmd(f"echo tcp | tee -a {port_path}/addr_trtype > /dev/null", args.real)
    exec_cmd(f"echo 4420 | tee -a {port_path}/addr_trsvcid > /dev/null", args.real)
    for i in range(args.num_devices):
        idx = args.start_device + i
        target_name = f"{args.target_name}-{idx}"
        target_path = f"/sys/kernel/config/nvmet/subsystems/{target_name}"
        exec_cmd(
            f"ln -s {target_path}/ {port_path}/subsystems/{target_name}", args.real
        )
    exec_cmd("dmesg | grep nvmet_tcp", args.real)
    exec_cmd("nvmetcli ls", args.real)


def setup_exporter(args):
    exec_cmd("modprobe nvmet", args.real)
    exec_cmd("modprobe nvmet-tcp", args.real)
    exec_cmd("/bin/mount -t configfs none /sys/kernel/config/", args.real)
    exec_cmd("lsblk", args.real)
    build_nvmetcli(args.real)
    if args.ipaddr:
        ip_addr = args.ipaddr
    else:
        ip_addr = run_cmd(["hostname", "-i"], for_real=True).splitlines()[0].strip()
        ip_addr = ip_addr.split(" ")[0]
    try:
        create_partitions(args)
        export_devices(args, ip_addr)
    except Exception as e:
        print(f"Encountered exception: {str(e)}")
        logger.info("Cleanup using nvmet-cli")
        exec_cmd("nvmet clear")
        logger.info("You may also want to cleanup any dangling partition")
        return
    else:
        logger.info("================ Exporter/target side setup done ================")
        logger.info("On importer side, run the following command to connect:")
        cmd = [
            __file__,
            "importer",
            "connect",
            "-n",
            str(args.num_devices),
            "-s",
            str(args.start_device),
            "-i",
            ip_addr,
            "-t",
            args.target_name,
            "--real",
        ]
        print(" ".join(cmd))


def setup_importer_connection(args):
    exec_cmd("modprobe nvme", args.real)
    exec_cmd("modprobe nvme-tcp", args.real)
    build_nvme_cli(args.real)
    exec_cmd("lsblk", args.real)
    hostnqn = run_cmd(["cat", "/etc/nvme/hostnqn"], for_real=True).strip()
    if os.path.exists("/etc/nvme/hostnqn") and hostnqn:
        pass
    else:
        exec_cmd("nvme gen-hostnqn | tee -a /etc/nvme/hostnqn > /dev/null", args.real)
    exec_cmd(f"nvme discover -t tcp -a {args.ipaddr} -s 4420", args.real)
    for i in range(args.num_devices):
        idx = args.start_device + i
        target_name = f"{args.target_name}-{idx}"
        connect_cmd = "nvme connect -t tcp -s 4420"
        exec_cmd(f"{connect_cmd} -a {args.ipaddr} -n {target_name}", args.real)
    time.sleep(1)
    exec_cmd("lsblk", args.real)
    logger.info("================ Client side connected ================")
    logger.info("Look at above output from lsblk to find imported devices")


def setup_importer_mount(args):
    exec_cmd("lsblk", args.real)
    device_list = [f"nvme{args.start_device + x}n1" for x in range(args.num_devices)]
    logger.info(", ".join([f"/dev/{x}" for x in device_list]))
    confirmation = input("Does above device list look correct? (yes/no): ")
    for_real = args.real and (confirmation == "yes")
    if confirmation != "yes":
        logger.info("!!!!!!!! Please use following commands as reference !!!!!!!!")
        logger.info("!!!!!!!! and apply to correct list of devices !!!!!!!!")
    # disable merges
    for x in range(args.num_devices):
        idx = args.start_device + x
        exec_cmd(f"echo 2 > /sys/block/nvme{idx}n1/queue/nomerges", for_real)
        exec_cmd(f"echo 2 > /sys/block/nvme{idx}c{idx}n1/queue/nomerges", for_real)
    md_idx = 23
    while os.path.exists(f"/dev/md{md_idx}"):
        md_idx += 1
    cmd = [
        "mdadm",
        "--create",
        f"/dev/md{md_idx}",
        "--level=0",
        f"--raid-devices={args.num_devices}",
        "--chunk=128",
    ]
    cmd.extend([f"/dev/{x}" for x in device_list])
    exec_cmd(" ".join(cmd), for_real)
    exec_cmd(f"mkfs.xfs -f /dev/md{md_idx}", for_real)
    point_idx = 23
    while os.path.exists(f"/flash{point_idx}"):
        point_idx += 1
    exec_cmd(f"mkdir /flash{point_idx}", for_real)
    exec_cmd(f"mount -o rw,noatime,discard /dev/md{md_idx} /flash{point_idx}", for_real)
    exec_cmd("lsblk", args.real)
    logger.info("================ Client side setup done ================")


def clear_from_importer(args):
    exec_cmd("lsblk", args.real)
    exec_cmd(f"umount /flash{args.flash_idx}/", args.real)
    exec_cmd(f"mdadm --stop /dev/md{args.md_idx}", args.real)
    for i in range(args.num_devices):
        idx = args.start_device + i
        exec_cmd(f"mdadm --zero-superblock /dev/nvme{idx}n1", args.real)
    for i in range(args.num_devices):
        idx = args.start_device + i
        exec_cmd(f"nvme disconnect -d /dev/nvme{idx}", args.real)
    exec_cmd("lsblk", args.real)


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    # level-1 sub-command parsers
    entity_parsers = parser.add_subparsers(help="Entity")
    exporter_parser = entity_parsers.add_parser(
        "exporter",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="exporter (target) side actions",
    )
    importer_parser = entity_parsers.add_parser(
        "importer",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="importer (initiator) side actions",
    )
    # level-2 sub-command parsers
    # exporter side
    exporter_action_parsers = exporter_parser.add_subparsers(help="Action")
    exporter_setup_parser = exporter_action_parsers.add_parser(
        "setup",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="exporter (target) setup",
    )
    exporter_setup_parser.set_defaults(func=setup_exporter)
    # importer side
    importer_action_parsers = importer_parser.add_subparsers(help="Action")
    importer_connect_parser = importer_action_parsers.add_parser(
        "connect",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="importer (initiator) connection setup",
    )
    importer_mount_parser = importer_action_parsers.add_parser(
        "mount",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="importer (initiator) mount setup",
    )
    importer_clear_parser = importer_action_parsers.add_parser(
        "clear",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        help="importer clean up",
    )
    importer_connect_parser.set_defaults(func=setup_importer_connection)
    importer_mount_parser.set_defaults(func=setup_importer_mount)
    importer_clear_parser.set_defaults(func=clear_from_importer)
    # common
    for x_parser in [
        exporter_setup_parser,
        importer_connect_parser,
        importer_mount_parser,
        importer_clear_parser,
    ]:
        x_parser.add_argument(
            "--num-devices",
            "-n",
            type=int,
            required=True,
            help="number of SSD devices to export",
        )
        x_parser.add_argument(
            "--start-device",
            "-s",
            type=int,
            required=True,
            help="start index of SSD; typically 0 is boot SDD & 1+ are data SSDs",
        )
    # exporter side
    default_target_name = f"nvmet-{socket.gethostname().split('.')[0]}"
    exporter_setup_parser.add_argument(
        "--partition-idx",
        "-p",
        type=int,
        required=True,
        help="should match your selection in fdisk prompt when creating new partition",
    )
    exporter_setup_parser.add_argument(
        "--target-name",
        "-t",
        type=str,
        default=default_target_name,
        help="target name prefix",
    )
    exporter_setup_parser.add_argument(
        "--ipv4",
        "-4",
        action="store_true",
        help="use ipv4",
    )
    exporter_setup_parser.add_argument(
        "--ipaddr",
        "-i",
        type=str,
        default="",
        help="IP addr of this exporter host",
    )
    # importer side
    importer_connect_parser.add_argument(
        "--ipaddr",
        "-i",
        type=str,
        required=True,
        help="IP addr of exporter/target host",
    )
    importer_connect_parser.add_argument(
        "--target-name", "-t", type=str, help="target name prefix"
    )
    importer_clear_parser.add_argument(
        "--flash-idx", "-f", type=int, default=23, help="/flash{?} index"
    )
    importer_clear_parser.add_argument(
        "--md-idx", "-m", type=int, default=23, help="/dev/md{?} index"
    )
    # as always, dry run or for real
    for x in [
        exporter_setup_parser,
        importer_connect_parser,
        importer_mount_parser,
        importer_clear_parser,
    ]:
        x.add_argument("--real", action="store_true", help="for real")
    return parser


if __name__ == "__main__":
    parser = init_parser()
    args = parser.parse_args()
    args.func(args)
