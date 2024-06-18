#!/usr/bin/env python3

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
    sys_configs["memory"] = int(mem_bytes / (1024.0**3))
    return sys_configs


def find_java_home() -> str:
    # Try finding a home path for java 8
    candidates = [
        "/usr/lib/jvm/java-1.8.0-openjdk",
        "/usr/lib/jvm/java-1.8.0-jre",
        "/usr/lib/jvm/java-8-openjdk",
        "/usr/lib/jvm/java-8-jre",
        "/usr/lib/jvm/openjdk-8",
        "/usr/lib/jvm/jre-1.8.0",
        "/usr/lib/jvm/jre-1.8.0-openjdk",
    ]
    archname = platform.machine()
    if archname == "x86_64":
        archname = "amd64"
    elif archname == "aarch64":
        archname = "arm64"
    for path in candidates:
        if os.path.exists(f"{path}/bin/java"):
            return path
        path_with_arch = f"{path}-{archname}"
        if os.path.exists(f"{path_with_arch}/bin/java"):
            return path_with_arch
    # If none of the candidate exists, try find through `java` command
    try:
        java_path = subprocess.check_output(["which", "java"], text=True).strip()
        java_home = str(pathlib.Path(os.path.realpath(java_path)).parents[2])
    except subprocess.CalledProcessError:
        java_home = ""

    return java_home


def read_environ() -> Dict[str, str]:
    # default env values
    env_vars = {}
    env_vars["PROJ_ROOT"] = "/".join(os.path.abspath(__file__).split("/")[:-2])
    env_vars["JAVA_HOME"] = find_java_home()
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
