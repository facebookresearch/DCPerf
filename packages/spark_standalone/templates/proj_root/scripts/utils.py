#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import pathlib
import platform
import subprocess
from typing import Dict, List, Optional


def exec_cmd(
    cmd: List[str],
    for_real: bool,
    print_cmd: bool = True,
) -> None:
    cmd_str = " ".join(cmd)
    if print_cmd:
        print(cmd_str)
    if for_real:
        os.system(cmd_str)


def launch_proc(cmd, cwd, stdout, stderr, env):
    return subprocess.Popen(
        cmd,
        cwd=cwd,
        stdout=stdout,
        stderr=stderr,
        env=env,
    )


def run_cmd(
    cmd: List[str],
    cwd: str,
    outfile: Optional[str],
    env: Dict[str, str],
    for_real: bool,
    print_cmd: bool = True,
    check: bool = True,
) -> Optional[str]:
    env_setting = [f"{k}={v}" for k, v in env.items()]
    cmd_str = " ".join(cmd)
    if print_cmd:
        print(" ".join(env_setting + cmd))
    exec_env = os.environ.copy()
    if for_real:
        for k, v in env.items():
            exec_env[k] = v
        if outfile:
            with open(outfile, "wt") as fp:
                proc = launch_proc(cmd, cwd, fp, fp, exec_env)
                proc.wait()
                if check and proc.returncode != 0:
                    raise RuntimeError(
                        f"Command failed with return code {proc.returncode}: {cmd_str}"
                    )
            return None
        else:
            proc = launch_proc(cmd, cwd, subprocess.PIPE, subprocess.STDOUT, exec_env)
            (stdout, _) = proc.communicate()
            if check and proc.returncode != 0:
                raise RuntimeError(
                    f"Command failed with return code {proc.returncode}: {cmd_str}"
                )
            return stdout.decode("utf-8")
    return None


def read_sys_configs() -> Dict[str, int]:
    # cpu core
    cmd = ["lscpu", "--json"]
    stdout = run_cmd(cmd, cwd=".", outfile=None, env={}, for_real=True, print_cmd=False)
    lscpu_out = json.loads(stdout)["lscpu"]
    sys_configs = {}
    for item in lscpu_out:
        if item["field"].startswith("Thread(s) per core"):
            sys_configs["threads_per_core"] = int(item["data"])
        if item["field"].startswith("Core(s) per socket"):
            sys_configs["cores_per_socket"] = int(item["data"])
        if item["field"].startswith("Socket(s)"):
            sys_configs["sockets"] = int(item["data"])
        if item["field"].startswith("Model name"):
            sys_configs["arch"] = item["data"]
    sys_configs["cores"] = (
        sys_configs["threads_per_core"]
        * sys_configs["cores_per_socket"]
        * sys_configs["sockets"]
    )
    # memory
    mem_bytes = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    sys_configs["memory"] = int(mem_bytes / (1024.0**3))
    return sys_configs


def find_java_home() -> str:
    # Always use the specific GraalVM JDK 17.0.12 path
    return "/usr/lib/jvm/graalvm-jdk-17.0.12+8.1"


def read_environ() -> Dict[str, str]:
    # default env values
    env_vars = {}
    env_vars["PROJ_ROOT"] = "/".join(os.path.abspath(__file__).split("/")[:-2])
    env_vars["JAVA_HOME"] = find_java_home()
    env_vars["SPARK_HOME"] = os.path.join(
        env_vars["PROJ_ROOT"], "spark-4.0.0-bin-hadoop3"
    )
    # read from actual environment
    for k in env_vars:
        try:
            env_vars[k] = os.environ[k]
        except KeyError:
            if os.path.exists(env_vars[k]):
                print(f"Using default {k} at {env_vars[k]}")
            else:
                print(f"Env var {k} not set & default path {env_vars[k]} not exist")
                exit(1)
        else:
            print(f"Using env {k} at {env_vars[k]}")
    return env_vars


def get_os_release() -> dict[str, str]:
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


if __name__ == "__main__":
    print(read_sys_configs())
    print(read_environ())
