#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import ipaddress
import json
import logging
import os
import re
import shlex
import socket
import subprocess
import time
from pathlib import Path

if __package__:
    from .utils import InfoLogFormat, is_distro_like, run_cmd, setup_logger
else:
    # README and Automark execute this file directly without package context.
    import importlib

    _utils = importlib.import_module("utils")
    InfoLogFormat = _utils.InfoLogFormat
    is_distro_like = _utils.is_distro_like
    run_cmd = _utils.run_cmd
    setup_logger = _utils.setup_logger


logger = setup_logger(__name__, logging.INFO, InfoLogFormat)


ENUM_INT_MISMATCH_FLAG = "-Wno-error=enum-int-mismatch"
DEFAULT_STATE_FILE = ".automark-spark-nvme-state.json"
NVME_SUBSYSTEM_PATH: Path = Path("/sys/class/nvme-subsystem")
_DEVICE_RE = re.compile(r"^/dev/nvme\d+n\d+$")
_TARGET_RE = re.compile(r"^[A-Za-z0-9._:-]+$")
_CONTROLLER_RE = re.compile(r"^nvme\d+$")
_NAMESPACE_RE = re.compile(r"^nvme\d+n\d+$")


def _run_checked(args, real, input_text=None, capture_output=False):
    logger.info(shlex.join(args))
    if not real:
        return ""
    try:
        result = subprocess.run(
            args,
            check=True,
            input=input_text,
            text=True,
            stdout=subprocess.PIPE if capture_output else None,
            stderr=subprocess.PIPE if capture_output else None,
        )
    except subprocess.CalledProcessError as error:
        if capture_output and error.stderr:
            logger.error(error.stderr.strip())
        raise
    if capture_output and result.stderr:
        logger.warning(result.stderr.strip())
    return result.stdout or ""


def compiler_supports_flag(flag):
    compiler = shlex.split(os.environ.get("CC", "cc"))
    try:
        result = subprocess.run(
            compiler + ["-x", "c", "-c", "-o", os.devnull, flag, "-"],
            input=b"int x;\n",
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return False
    return result.returncode == 0


def build_nvme_cli(real):
    if is_distro_like("centos"):
        _run_checked(["dnf", "install", "-y", "libuuid-devel"], real)
    elif is_distro_like("ubuntu"):
        _run_checked(["apt", "install", "-y", "libuuid1", "uuid-dev"], real)
    root_path = os.getcwd()
    cli_util_path = os.path.join(root_path, "nvme-cli")
    if not os.path.exists(cli_util_path):
        if not real:
            logger.info(f"would clone and build {cli_util_path}")
            return
        _run_checked(["git", "clone", "https://github.com/pkuwangh/nvme-cli"], real)
    logger.info(f"chdir to {cli_util_path}")
    try:
        os.chdir(cli_util_path)
        _run_checked(
            ["git", "checkout", "274a49759c8cbdd991253455c64136e0ea73cb6b"],
            real,
        )
        _run_checked(["make", "clean"], real)
        make_args = ["make"]
        if compiler_supports_flag(ENUM_INT_MISMATCH_FLAG):
            make_args.append(f"CFLAGS+={ENUM_INT_MISMATCH_FLAG}")
        _run_checked(make_args, real)
        _run_checked(["make", "install"], real)
    finally:
        logger.info(f"chdir to {root_path}")
        os.chdir(root_path)
    _run_checked(["nvme", "list"], real)


def build_nvmetcli(real):
    if is_distro_like("centos"):
        _run_checked(["dnf", "install", "-y", "python3-configshell"], real)
    elif is_distro_like("ubuntu"):
        _run_checked(["apt", "install", "-y", "python3-configshell-fb"], real)
    root_path = os.getcwd()
    cli_util_path = os.path.join(root_path, "nvmetcli")
    if not os.path.exists(cli_util_path):
        if not real:
            logger.info(f"would clone and build {cli_util_path}")
            return
        _run_checked(
            [
                "git",
                "clone",
                "https://github.com/pkuwangh/nvmetcli.git",
                "-b",
                "for_spark",
            ],
            real,
        )
    logger.info(f"chdir to {cli_util_path}")
    try:
        os.chdir(cli_util_path)
        _run_checked(
            ["git", "checkout", "7655e1020bfa15f89aa4df125cf3772c23c86396"],
            real,
        )
        _run_checked(["./setup.py", "install"], real)
    finally:
        logger.info(f"chdir to {root_path}")
        os.chdir(root_path)
    _run_checked(["nvmetcli", "ls"], real)


def _parse_device_indices(args):
    selectors = sum(
        [
            bool(args.device_indices),
            args.start_device is not None,
            args.auto_devices,
        ]
    )
    if selectors != 1:
        raise ValueError(
            "exactly one of --start-device, --device-indices, or --auto-devices "
            "is required"
        )
    if args.auto_devices:
        return _discover_data_device_indices(args.num_devices, args.real)
    if args.device_indices:
        try:
            indices = [int(value) for value in args.device_indices.split(",")]
        except ValueError as error:
            raise ValueError(
                "device indices must be comma-separated integers"
            ) from error
        if len(indices) != args.num_devices:
            raise ValueError(
                f"expected {args.num_devices} device indices, got {len(indices)}"
            )
    else:
        indices = list(range(args.start_device, args.start_device + args.num_devices))
    if any(index < 0 for index in indices) or len(set(indices)) != len(indices):
        raise ValueError("device indices must be unique non-negative integers")
    return indices


def _target_names(target_prefix, num_devices, start_device=0):
    if not target_prefix or not _TARGET_RE.fullmatch(target_prefix):
        raise ValueError(f"invalid target prefix: {target_prefix!r}")
    return [
        f"{target_prefix}-{index}"
        for index in range(start_device, start_device + num_devices)
    ]


def _export_state_path(target_prefix):
    _target_names(target_prefix, 0)
    return Path(f".{target_prefix}-nvmet-state.json")


def _flatten_lsblk(nodes):
    if not isinstance(nodes, list):
        raise RuntimeError("invalid lsblk JSON: block device list is malformed")
    for node in nodes:
        if not isinstance(node, dict):
            raise RuntimeError("invalid lsblk JSON: block device entry is malformed")
        yield node
        yield from _flatten_lsblk(node.get("children", []))


def _mounts_from_lsblk_nodes(nodes):
    mounts = []
    for node in nodes:
        mountpoints = node.get("mountpoints")
        if mountpoints is None:
            mountpoints = [node.get("mountpoint")]
        if not isinstance(mountpoints, list):
            raise RuntimeError("invalid lsblk JSON: mountpoints must be a list")
        mounts.extend(mountpoint for mountpoint in mountpoints if mountpoint)
    return mounts


def _is_local_pcie_device(device):
    match = re.fullmatch(r"/dev/nvme(\d+)n1", device)
    if match is None:
        return False
    transport = Path(f"/sys/class/nvme/nvme{match.group(1)}/transport")
    return transport.exists() and transport.read_text().strip() == "pcie"


def _discover_data_device_indices(num_devices, real):  # noqa: C901
    if not real:
        logger.info(f"would auto-discover {num_devices} local NVMe data devices")
        return list(range(num_devices))
    try:
        output = _run_checked(
            ["lsblk", "--json", "--paths", "--output", "NAME,PATH,TYPE,MOUNTPOINTS"],
            real=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError:
        output = _run_checked(
            ["lsblk", "--json", "--paths", "--output", "NAME,PATH,TYPE,MOUNTPOINT"],
            real=True,
            capture_output=True,
        )
    try:
        payload = json.loads(output)
    except json.JSONDecodeError as error:
        raise RuntimeError("invalid lsblk JSON output") from error
    if not isinstance(payload, dict):
        raise RuntimeError("invalid lsblk JSON: expected an object")
    block_devices = payload.get("blockdevices", [])
    candidates = []
    for node in _flatten_lsblk(block_devices):
        if node.get("type") != "disk":
            continue
        device = node.get("path")
        if not isinstance(device, str) or not _is_local_pcie_device(device):
            continue
        if _mounts_from_lsblk_nodes(list(_flatten_lsblk([node]))):
            continue
        match = re.fullmatch(r"/dev/nvme(\d+)n1", device)
        if match is None:
            continue
        _validate_data_device(device, real=True)
        candidates.append(int(match.group(1)))
    candidates.sort()
    if len(candidates) != num_devices:
        raise RuntimeError(
            f"expected exactly {num_devices} unmounted local NVMe data devices, "
            f"found {candidates}"
        )
    logger.info(f"Auto-discovered Spark data device indices: {candidates}")
    return candidates


def _validate_data_device(device, real):  # noqa: C901
    if not _DEVICE_RE.fullmatch(device):
        raise ValueError(f"invalid NVMe data device: {device}")
    if not real:
        return []
    if not Path(device).exists():
        raise RuntimeError(f"NVMe data device does not exist: {device}")
    try:
        output = _run_checked(
            [
                "lsblk",
                "--json",
                "--paths",
                "--output",
                "NAME,PATH,TYPE,MOUNTPOINTS",
                device,
            ],
            real=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError:
        output = _run_checked(
            [
                "lsblk",
                "--json",
                "--paths",
                "--output",
                "NAME,PATH,TYPE,MOUNTPOINT",
                device,
            ],
            real=True,
            capture_output=True,
        )
    try:
        payload = json.loads(output)
    except json.JSONDecodeError as error:
        raise RuntimeError("invalid lsblk JSON output") from error
    if not isinstance(payload, dict):
        raise RuntimeError("invalid lsblk JSON: expected an object")
    nodes = list(_flatten_lsblk(payload.get("blockdevices", [])))
    root_source = _run_checked(
        ["findmnt", "-n", "-o", "SOURCE", "--target", "/"],
        real=True,
        capture_output=True,
    ).strip()
    if root_source == device or root_source.startswith(f"{device}p"):
        raise RuntimeError(f"refusing root filesystem NVMe device: {device}")
    mounts = _mounts_from_lsblk_nodes(nodes)
    if "/" in mounts:
        raise RuntimeError(f"refusing root filesystem NVMe device: {device}")
    if mounts:
        raise RuntimeError(f"refusing mounted NVMe data device {device}: {mounts}")
    holder_dir = Path("/sys/class/block") / Path(device).name / "holders"
    if holder_dir.exists() and any(holder_dir.iterdir()):
        raise RuntimeError(f"refusing NVMe data device with active holders: {device}")
    partitions = []
    for node in nodes:
        if node.get("type") != "part":
            continue
        path = node.get("path")
        if not isinstance(path, str):
            raise RuntimeError("invalid lsblk JSON: partition path is missing")
        partitions.append(path)
    return partitions


def _ensure_partition(device, partition_index, real):
    partitions = _validate_data_device(device, real)
    partition = f"{device}p{partition_index}"
    if not real:
        logger.info(f"would ensure partition {partition}")
        return partition
    if partition in partitions:
        if len(partitions) != 1:
            raise RuntimeError(
                f"refusing unexpected partition layout on {device}: {partitions}"
            )
        _run_checked(["wipefs", "--all", "--force", partition], real=True)
        return partition
    if partitions:
        raise RuntimeError(
            f"refusing to replace unexpected partitions on {device}: {partitions}"
        )
    if partition_index != 1:
        raise RuntimeError("automatic partition creation supports only partition 1")
    signatures = _run_checked(
        ["wipefs", "--noheadings", "--output", "TYPE", device],
        real=True,
        capture_output=True,
    ).strip()
    if signatures:
        raise RuntimeError(
            f"refusing signatures on unpartitioned {device}: {signatures}"
        )
    _run_checked(
        ["sfdisk", "--label", "gpt", device],
        real=True,
        input_text=",,L\n",
    )
    _run_checked(["partprobe", device], real=True)
    _run_checked(["udevadm", "settle"], real=True)
    if not Path(partition).exists():
        raise RuntimeError(f"partition creation did not produce {partition}")
    return partition


def create_partitions(args):
    partitions = []
    for index in _parse_device_indices(args):
        device = f"/dev/nvme{index}n1"
        logger.info(f"Preparing Spark data device {device}")
        partitions.append(_ensure_partition(device, args.partition_idx, args.real))
    if args.real:
        _run_checked(["lsblk"], real=True)
    return partitions


def _write_configfs(path, value, real):
    logger.info(f"write {value!r} to {path}")
    if real:
        path.write_text(value)


def _cleanup_exported_targets(target_names, real):
    port_path = Path("/sys/kernel/config/nvmet/ports/1")
    for target_name in reversed(target_names):
        link = port_path / "subsystems" / target_name
        target_path = Path("/sys/kernel/config/nvmet/subsystems") / target_name
        namespace = target_path / "namespaces/1"
        logger.info(f"remove NVMe target {target_name}")
        if not real:
            continue
        if link.is_symlink():
            link.unlink()
        enable = namespace / "enable"
        if enable.exists():
            enable.write_text("0")
        if namespace.exists():
            namespace.rmdir()
        if target_path.exists():
            target_path.rmdir()
    if real and port_path.exists():
        remaining_links = list((port_path / "subsystems").iterdir())
        if remaining_links:
            raise RuntimeError(f"refusing shared NVMe target port: {remaining_links}")
        port_path.rmdir()


def export_devices(args, ip_addr, partitions):
    target_start = 0 if args.device_indices or args.auto_devices else args.start_device
    target_names = _target_names(args.target_name, len(partitions), target_start)
    state_path = _export_state_path(args.target_name)
    if args.real and state_path.exists():
        raise RuntimeError(f"refusing existing NVMe export state: {state_path}")
    created_targets = []
    port_path = Path("/sys/kernel/config/nvmet/ports/1")
    if args.real and port_path.exists():
        raise RuntimeError(f"refusing pre-existing NVMe target port: {port_path}")
    try:
        if args.real:
            port_path.mkdir(parents=True)
        _write_configfs(port_path / "addr_traddr", ip_addr, args.real)
        _write_configfs(
            port_path / "addr_adrfam", "ipv4" if args.ipv4 else "ipv6", args.real
        )
        _write_configfs(port_path / "addr_trtype", "tcp", args.real)
        _write_configfs(port_path / "addr_trsvcid", "4420", args.real)

        for target_name, partition in zip(target_names, partitions):
            target_path = Path("/sys/kernel/config/nvmet/subsystems") / target_name
            namespace = target_path / "namespaces/1"
            link = port_path / "subsystems" / target_name
            if args.real and (target_path.exists() or link.exists()):
                raise RuntimeError(f"NVMe target already exists: {target_name}")
            if args.real:
                namespace.mkdir(parents=True)
                created_targets.append(target_name)
            _write_configfs(target_path / "attr_allow_any_host", "1", args.real)
            _write_configfs(namespace / "device_path", partition, args.real)
            _write_configfs(namespace / "enable", "1", args.real)
            logger.info(f"link {target_path} to {link}")
            if args.real:
                os.symlink(target_path, link)
        if args.real:
            _run_checked(["nvmetcli", "ls"], real=True)
            _write_state(state_path, {"target_names": target_names})
    except Exception:
        _cleanup_exported_targets(created_targets, args.real)
        raise
    return target_names


def _require_ipv4_address(address):
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError as error:
        raise RuntimeError(f"expected an IPv4 address, got {address!r}") from error
    if parsed.version != 4:
        raise RuntimeError(f"expected an IPv4 address, got {address!r}")
    return str(parsed)


def _get_local_ipv4_address():
    output = _run_checked(["hostname", "-I"], real=True, capture_output=True).split()
    for address in output:
        try:
            parsed = ipaddress.ip_address(address)
        except ValueError:
            continue
        if parsed.version == 4 and not parsed.is_loopback:
            return str(parsed)
    raise RuntimeError("could not determine a local IPv4 address")


def setup_exporter(args):
    _run_checked(["modprobe", "nvmet"], args.real)
    _run_checked(["modprobe", "nvmet-tcp"], args.real)
    if args.real and not os.path.ismount("/sys/kernel/config"):
        _run_checked(
            ["mount", "-t", "configfs", "none", "/sys/kernel/config"], real=True
        )
    build_nvmetcli(args.real)
    if args.ipaddr:
        ip_addr = args.ipaddr
    elif args.ipv4:
        ip_addr = _get_local_ipv4_address()
    else:
        ip_addr = run_cmd(["hostname", "-i"], for_real=True).splitlines()[0].strip()
        ip_addr = ip_addr.split(" ")[0]
    if args.ipv4:
        ip_addr = _require_ipv4_address(ip_addr)
    partitions = create_partitions(args)
    target_names = export_devices(args, ip_addr, partitions)
    logger.info("================ Exporter/target side setup done ================")
    logger.info(f"Exported targets: {', '.join(target_names)}")


def clear_exporter(args):
    state_path = _export_state_path(args.target_name)
    if not args.real:
        logger.info(f"would clean exporter state from {state_path}")
        return
    if not state_path.exists():
        logger.info(f"No exporter state file at {state_path}; nothing to clean")
        return
    state = json.loads(state_path.read_text())
    target_names = state.get("target_names")
    expected = re.compile(rf"^{re.escape(args.target_name)}-\d+$")
    if not isinstance(target_names, list) or not all(
        isinstance(name, str) and expected.fullmatch(name) for name in target_names
    ):
        raise RuntimeError(f"invalid NVMe exporter state at {state_path}")
    _cleanup_exported_targets(target_names, args.real)
    state_path.unlink()


def _namespace_names(subsystem):
    namespaces = {
        entry.name
        for entry in subsystem.iterdir()
        if _NAMESPACE_RE.fullmatch(entry.name)
    }
    controllers = [
        entry for entry in subsystem.iterdir() if _CONTROLLER_RE.fullmatch(entry.name)
    ]
    for controller in controllers:
        namespaces.update(
            entry.name
            for entry in controller.iterdir()
            if _NAMESPACE_RE.fullmatch(entry.name)
        )
    return sorted(namespaces)


def _find_target_devices(target_names, sysfs_root=NVME_SUBSYSTEM_PATH):
    expected = set(target_names)
    found = {}
    if not sysfs_root.exists():
        return found
    for subsystem in sysfs_root.glob("nvme-subsys*"):
        nqn_path = subsystem / "subsysnqn"
        if not nqn_path.exists():
            continue
        nqn = nqn_path.read_text().strip()
        if nqn not in expected:
            continue
        if nqn in found:
            raise RuntimeError(f"duplicate NVMe subsystem for target {nqn}")
        namespaces = _namespace_names(subsystem)
        if not namespaces:
            continue
        if len(namespaces) != 1:
            raise RuntimeError(
                f"expected one namespace for target {nqn}, found {namespaces}"
            )
        found[nqn] = f"/dev/{namespaces[0]}"
    return found


def _wait_for_target_devices(target_names, timeout_seconds=30):
    deadline = time.time() + timeout_seconds
    while True:
        found = _find_target_devices(target_names)
        if set(found) == set(target_names):
            return [found[target_name] for target_name in target_names]
        if time.time() >= deadline:
            missing = sorted(set(target_names) - set(found))
            raise RuntimeError(f"NVMe targets did not appear: {missing}")
        time.sleep(0.25)


def _wait_for_target_absence(target_names, timeout_seconds=30):
    deadline = time.time() + timeout_seconds
    while True:
        remaining = _find_target_devices(target_names)
        if not remaining:
            return
        if time.time() >= deadline:
            raise RuntimeError(f"NVMe targets did not disconnect: {sorted(remaining)}")
        time.sleep(0.25)


def setup_importer_connection(args):
    _run_checked(["modprobe", "nvme"], args.real)
    _run_checked(["modprobe", "nvme-tcp"], args.real)
    build_nvme_cli(args.real)
    hostnqn_path = Path("/etc/nvme/hostnqn")
    if args.real and (
        not hostnqn_path.exists() or not hostnqn_path.read_text().strip()
    ):
        hostnqn = _run_checked(
            ["nvme", "gen-hostnqn"], real=True, capture_output=True
        ).strip()
        hostnqn_path.parent.mkdir(parents=True, exist_ok=True)
        hostnqn_path.write_text(f"{hostnqn}\n")
    ip_addr = _require_ipv4_address(args.ipaddr) if args.ipv4 else args.ipaddr
    _run_checked(
        ["nvme", "discover", "-t", "tcp", "-a", ip_addr, "-s", "4420"],
        args.real,
    )
    target_names = _target_names(args.target_name, args.num_devices, args.start_device)
    state = _read_state(args.state_file)
    if set(target_names) & set(state["target_names"]):
        raise RuntimeError(f"targets already recorded in Spark state: {target_names}")
    if args.real and _find_target_devices(target_names):
        raise RuntimeError(f"refusing pre-existing NVMe targets: {target_names}")
    connected_targets = []
    try:
        for target_name in target_names:
            _run_checked(
                [
                    "nvme",
                    "connect",
                    "-t",
                    "tcp",
                    "-s",
                    "4420",
                    "-a",
                    ip_addr,
                    "-n",
                    target_name,
                ],
                args.real,
            )
            if args.real:
                connected_targets.append(target_name)
        if args.real:
            devices = _wait_for_target_devices(target_names)
            _write_state(
                args.state_file,
                {
                    **state,
                    "devices": [*state["devices"], *devices],
                    "target_names": [*state["target_names"], *target_names],
                },
            )
            logger.info(f"Connected targets: {dict(zip(target_names, devices))}")
    except Exception:
        for target_name in connected_targets:
            subprocess.run(["nvme", "disconnect", "-n", target_name], check=False)
        raise


def _write_state(path, state):
    destination = Path(path)
    temporary = destination.with_suffix(f"{destination.suffix}.tmp")
    temporary.write_text(json.dumps(state, sort_keys=True))
    temporary.replace(destination)


def _read_state(path):
    state_path = Path(path)
    if not state_path.exists():
        return {
            "devices": [],
            "md_device": None,
            "mountpoint": None,
            "target_names": [],
        }
    state = json.loads(state_path.read_text())
    required = {"devices", "md_device", "mountpoint", "target_names"}
    if set(state) != required:
        raise RuntimeError(f"invalid Spark setup state at {state_path}")
    devices = state["devices"]
    target_names = state["target_names"]
    md_device = state["md_device"]
    mountpoint = state["mountpoint"]
    if (
        not isinstance(devices, list)
        or not all(isinstance(device, str) for device in devices)
        or not isinstance(target_names, list)
        or not all(isinstance(target_name, str) for target_name in target_names)
        or (md_device is not None and not isinstance(md_device, str))
        or (mountpoint is not None and not isinstance(mountpoint, str))
        or (target_names and len(devices) != len(target_names))
        or (mountpoint is not None and md_device is None)
    ):
        raise RuntimeError(f"invalid Spark setup state at {state_path}")
    return state


def _disable_merges(devices, real, sysfs_root=None):
    block_root = sysfs_root or Path("/sys/class/block")
    for device in devices:
        match = re.fullmatch(r"/dev/(nvme(\d+)n(\d+))", device)
        if match is None:
            raise ValueError(f"invalid NVMe data device: {device}")
        queue_paths = [block_root / match.group(1) / "queue" / "nomerges"]
        queue_paths.extend(
            entry / "queue" / "nomerges"
            for entry in sorted(
                block_root.glob(f"nvme{match.group(2)}c*n{match.group(3)}")
            )
        )
        for index, path in enumerate(queue_paths):
            logger.info(f"write '2' to {path}")
            if not real:
                continue
            if not path.exists():
                if index == 0:
                    raise RuntimeError(f"NVMe nomerges control does not exist: {path}")
                continue
            path.write_text("2")
            if path.read_text().strip() != "2":
                raise RuntimeError(f"NVMe nomerges postcondition failed: {path}")


def _setup_legacy_importer_mount(args):
    if args.num_devices is None or args.start_device is None:
        raise ValueError("legacy mount requires --num-devices and --start-device")
    devices = [
        f"/dev/nvme{args.start_device + offset}n1" for offset in range(args.num_devices)
    ]
    for device in devices:
        _validate_data_device(device, args.real)
    logger.info(f"Legacy importer devices: {', '.join(devices)}")
    if input("Does above device list look correct? (yes/no): ") != "yes":
        raise RuntimeError("importer device list was not confirmed")
    md_idx = args.md_idx
    while Path(f"/dev/md{md_idx}").exists():
        md_idx += 1
    flash_idx = args.flash_idx
    while Path(f"/flash{flash_idx}").exists():
        flash_idx += 1
    md_device = f"/dev/md{md_idx}"
    mountpoint = Path(f"/flash{flash_idx}")
    _disable_merges(devices, args.real)
    for device in devices:
        _run_checked(["wipefs", "--all", "--force", device], args.real)
    _run_checked(
        [
            "mdadm",
            "--create",
            "--run",
            md_device,
            "--level=0",
            f"--raid-devices={len(devices)}",
            "--chunk=128",
            *devices,
        ],
        args.real,
    )
    _run_checked(["mkfs.xfs", "-f", md_device], args.real)
    if args.real:
        mountpoint.mkdir()
    _run_checked(
        ["mount", "-o", "rw,noatime,discard", md_device, str(mountpoint)], args.real
    )
    if args.real:
        _write_state(
            args.state_file,
            {
                "devices": devices,
                "md_device": md_device,
                "mountpoint": str(mountpoint),
                "target_names": [],
            },
        )


def setup_importer_mount(args):  # noqa: C901
    state = _read_state(args.state_file)
    if args.target_names:
        target_names = [name for name in args.target_names.split(",") if name]
    elif state["target_names"]:
        target_names = state["target_names"]
        if args.num_devices is not None and args.num_devices != len(target_names):
            raise RuntimeError("recorded target count does not match --num-devices")
    else:
        return _setup_legacy_importer_mount(args)
    if len(target_names) != len(set(target_names)):
        raise ValueError("target names must be unique")
    devices = _wait_for_target_devices(target_names) if args.real else ["<tcp-device>"]
    if args.real:
        if not state["target_names"] and not state["devices"]:
            state = {
                "devices": devices,
                "md_device": None,
                "mountpoint": None,
                "target_names": target_names,
            }
            _write_state(args.state_file, state)
        elif state["target_names"] != target_names or state["devices"] != devices:
            raise RuntimeError(
                "connected NVMe devices do not match recorded Spark state"
            )
    for device in devices if args.real else []:
        _validate_data_device(device, real=True)

    md_device = f"/dev/md{args.md_idx}"
    mountpoint = Path(f"/flash{args.flash_idx}")
    if args.real and Path(md_device).exists():
        raise RuntimeError(f"refusing existing RAID device: {md_device}")
    if args.real and mountpoint.exists():
        raise RuntimeError(f"refusing existing Spark mount path: {mountpoint}")

    _disable_merges(devices, args.real)
    md_created = False
    mount_created = False
    try:
        for device in devices:
            _run_checked(["wipefs", "--all", "--force", device], args.real)
        _run_checked(
            [
                "mdadm",
                "--create",
                "--run",
                md_device,
                "--level=0",
                f"--raid-devices={len(devices)}",
                "--chunk=128",
                *devices,
            ],
            args.real,
        )
        md_created = args.real
        _run_checked(["mkfs.xfs", "-f", md_device], args.real)
        if args.real:
            mountpoint.mkdir()
            mount_created = True
        _run_checked(
            ["mount", "-o", "rw,noatime,discard", md_device, str(mountpoint)],
            args.real,
        )
        if args.real:
            source = _run_checked(
                ["findmnt", "-n", "-o", "SOURCE", "--target", str(mountpoint)],
                real=True,
                capture_output=True,
            ).strip()
            if (
                not source
                or not Path(source).exists()
                or not os.path.samefile(source, md_device)
            ):
                raise RuntimeError(
                    f"Spark mount postcondition failed: {mountpoint} is backed by {source!r}"
                )
            _write_state(
                args.state_file,
                {
                    "devices": devices,
                    "md_device": md_device,
                    "mountpoint": str(mountpoint),
                    "target_names": target_names,
                },
            )
    except Exception:
        if args.real:
            subprocess.run(["umount", str(mountpoint)], check=False)
            if md_created:
                subprocess.run(["mdadm", "--stop", md_device], check=False)
                for device in devices:
                    subprocess.run(["mdadm", "--zero-superblock", device], check=False)
            if mount_created and mountpoint.exists():
                mountpoint.rmdir()
        raise
    logger.info("================ Client side setup done ================")


def _find_mount_source(mountpoint):
    result = subprocess.run(
        ["findmnt", "-n", "-o", "SOURCE", "--target", mountpoint],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode == 1:
        return ""
    if result.returncode != 0:
        raise RuntimeError(f"findmnt failed for {mountpoint}: {result.stdout.strip()}")
    return result.stdout.strip()


def _md_members(md_device):
    slaves = Path("/sys/class/block") / Path(md_device).name / "slaves"
    if not slaves.exists():
        return []
    return sorted(f"/dev/{entry.name}" for entry in slaves.iterdir())


def clear_from_importer(args):  # noqa: C901
    state_path = Path(args.state_file)
    if not args.real:
        logger.info(f"would clean importer state from {state_path}")
        return
    if not state_path.exists():
        logger.info(f"No importer state file at {state_path}; nothing to clean")
        return
    state = _read_state(args.state_file)
    mountpoint = state["mountpoint"]
    md_device = state["md_device"]
    devices = state["devices"]
    target_names = state["target_names"]
    connected = _find_target_devices(target_names)
    if md_device is not None and target_names:
        current_devices = [connected.get(target_name) for target_name in target_names]
        remapped = [
            (target_name, recorded, current)
            for target_name, recorded, current in zip(
                target_names, devices, current_devices
            )
            if current is not None and current != recorded
        ]
        if remapped:
            raise RuntimeError(
                f"refusing cleanup: target NQNs map to different devices: {remapped}"
            )
        if Path(md_device).exists() and current_devices != devices:
            raise RuntimeError(
                "refusing cleanup: active RAID has missing NVMe target mappings"
            )
    md_exists = md_device is not None and Path(md_device).exists()
    md_members = _md_members(md_device) if md_exists else []
    if md_exists and sorted(devices) != md_members:
        raise RuntimeError(
            f"refusing cleanup: RAID members do not match state: {md_members}"
        )
    members_to_zero = md_members
    if (
        not md_exists
        and md_device is not None
        and target_names
        and current_devices == devices
    ):
        members_to_zero = devices
    if mountpoint is not None:
        source = _find_mount_source(mountpoint)
        if source:
            if not Path(source).exists() or not os.path.samefile(source, md_device):
                raise RuntimeError(
                    f"refusing cleanup: {mountpoint} is backed by unexpected {source!r}"
                )
            _run_checked(["umount", mountpoint], real=True)
    if md_exists:
        _run_checked(["mdadm", "--stop", md_device], real=True)
    for device in members_to_zero:
        _run_checked(["mdadm", "--zero-superblock", device], real=True)
    for target_name in target_names:
        if target_name in connected:
            subprocess.run(["nvme", "disconnect", "-n", target_name], check=False)
    if target_names:
        _wait_for_target_absence(target_names)
    if mountpoint is not None:
        mount_path = Path(mountpoint)
        if mount_path.exists():
            mount_path.rmdir()
    state_path.unlink()


def init_parser():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    entity_parsers = parser.add_subparsers(help="Entity", required=True)
    exporter_parser = entity_parsers.add_parser("exporter", help="target actions")
    importer_parser = entity_parsers.add_parser("importer", help="initiator actions")

    exporter_actions = exporter_parser.add_subparsers(help="Action", required=True)
    exporter_setup = exporter_actions.add_parser("setup", help="target setup")
    exporter_clear = exporter_actions.add_parser("clear", help="target cleanup")
    exporter_setup.set_defaults(func=setup_exporter)
    exporter_clear.set_defaults(func=clear_exporter)

    importer_actions = importer_parser.add_subparsers(help="Action", required=True)
    importer_connect = importer_actions.add_parser("connect", help="connect targets")
    importer_mount = importer_actions.add_parser("mount", help="create RAID and mount")
    importer_clear = importer_actions.add_parser("clear", help="cleanup recorded setup")
    importer_connect.set_defaults(func=setup_importer_connection)
    importer_mount.set_defaults(func=setup_importer_mount)
    importer_clear.set_defaults(func=clear_from_importer)

    for action in [exporter_setup, importer_connect]:
        action.add_argument("-n", "--num-devices", type=int, required=True)
    exporter_setup.add_argument("-s", "--start-device", type=int)
    importer_connect.add_argument("-s", "--start-device", type=int, required=True)
    exporter_setup.add_argument("--device-indices", default="")
    exporter_setup.add_argument("--auto-devices", action="store_true")
    exporter_setup.add_argument("-p", "--partition-idx", type=int, required=True)
    exporter_setup.add_argument(
        "-t", "--target-name", default=f"nvmet-{socket.gethostname().split('.')[0]}"
    )
    exporter_setup.add_argument("-4", "--ipv4", action="store_true")
    importer_connect.add_argument("-4", "--ipv4", action="store_true")
    exporter_setup.add_argument("-i", "--ipaddr", default="")
    exporter_clear.add_argument("-t", "--target-name", required=True)

    importer_connect.add_argument("-i", "--ipaddr", required=True)
    importer_connect.add_argument("-t", "--target-name", required=True)
    importer_mount.add_argument("-n", "--num-devices", type=int)
    importer_mount.add_argument("-s", "--start-device", type=int)
    importer_mount.add_argument("--target-names", default="")
    importer_mount.add_argument("--flash-idx", type=int, default=23)
    importer_mount.add_argument("--md-idx", type=int, default=23)
    for action in [importer_connect, importer_mount, importer_clear]:
        action.add_argument("--state-file", default=DEFAULT_STATE_FILE)

    for action in [
        exporter_setup,
        exporter_clear,
        importer_connect,
        importer_mount,
        importer_clear,
    ]:
        action.add_argument("--real", action="store_true", help="apply changes")
    return parser


if __name__ == "__main__":
    args = init_parser().parse_args()
    args.func(args)
