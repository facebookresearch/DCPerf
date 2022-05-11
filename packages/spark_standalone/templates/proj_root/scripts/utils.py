#!/usr/bin/env python3

import json
import os
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
    outfile: str,
    env: Dict[str, str],
    for_real: bool,
    print_cmd: bool = True,
) -> Optional[str]:
    env_setting = [f"{k}={v}" for k, v in env.items()]
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
            return None
        else:
            proc = launch_proc(cmd, cwd, subprocess.PIPE, subprocess.STDOUT, exec_env)
            (stdout, _) = proc.communicate()
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
    sys_configs["memory"] = int(mem_bytes / (1024.0 ** 3))
    return sys_configs


def read_environ() -> Dict[str, str]:
    # default env values
    env_vars = {}
    env_vars["PROJ_ROOT"] = "/".join(os.path.abspath(__file__).split("/")[:-2])
    env_vars["JAVA_HOME"] = "/usr/lib/jvm/jre-1.8.0-openjdk"
    env_vars["SPARK_HOME"] = os.path.join(
        env_vars["PROJ_ROOT"], "spark-2.4.5-bin-hadoop2.7"
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


if __name__ == "__main__":
    print(read_sys_configs())
    print(read_environ())
