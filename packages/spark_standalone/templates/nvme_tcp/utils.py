#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import logging
import logging.config
import os
import subprocess
from typing import Dict, List


CommandLogFormat = "\033[36m>>>> %(message)s\033[0m"
InfoLogFormat = "\033[33mInfo: %(message)s\033[0m"


def setup_logger(name, level, content_format):
    cfg = {
        "version": 1,
        "disable_existing_loggers": False,
        "formatters": {f"default_for_{name}": {"format": content_format}},
        "handlers": {
            f"console_for_{name}": {
                "class": "logging.StreamHandler",
                "level": level,
                "formatter": f"default_for_{name}",
                "stream": "ext://sys.stdout",
            },
        },
        "loggers": {name: {"level": level, "handlers": [f"console_for_{name}"]}},
    }
    logging.config.dictConfig(cfg)
    return logging.getLogger(name)


logger = setup_logger(__name__, logging.INFO, CommandLogFormat)


def exec_cmd(
    cmd_str: str,
    for_real: bool,
    print_cmd: bool = True,
) -> None:
    if print_cmd:
        logger.info(cmd_str)
    if for_real:
        os.system(cmd_str)


def run_cmd(
    cmd: List[str],
    for_real: bool,
    print_cmd: bool = True,
) -> str:
    if print_cmd:
        print(" ".join(cmd))
    if for_real:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (stdout, _) = proc.communicate()
        return stdout.decode("utf-8")
    return ""


def get_os_release() -> Dict[str, str]:
    if not os.path.exists("/etc/os-release"):
        return {}
    with open("/etc/os-release", "r") as f:
        os_release_text = f.read()
    os_release = {}
    for line in os_release_text.splitlines():
        key, value = line.split("=", maxsplit=1)
        value = value.strip('"')
        value = value.strip()
        os_release[key] = value

    return os_release


def is_distro_like(distro_id: str) -> bool:
    os_release = get_os_release()
    ids = []
    if "ID" in os_release.keys():
        ids.append(os_release["ID"])
    if "ID_LIKE" in os_release.keys():
        ids.extend(os_release["ID_LIKE"].split(" "))
    return distro_id in ids
