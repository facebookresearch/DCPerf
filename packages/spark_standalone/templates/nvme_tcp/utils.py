#!/usr/bin/env python3

import logging
import logging.config
import os
import subprocess
from typing import List


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
