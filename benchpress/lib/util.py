#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import errno
import multiprocessing
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import typing
import uuid

import click


BENCHMARKS_ROOT = "benchmarks"


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def get_safe_cmd(arg_list):
    """
    Allow command to be run by subprocess with shell=False
    """
    safe_args = []
    for arg in arg_list:
        # ''.split(' ') returns [''] instead of []
        for sub_arg in arg.split():
            safe_args.append(sub_arg)
    return safe_args


def issue_background_command(cmd, stdout, stderr, env=None):
    if not stdout or not stderr:
        stdout = subprocess.PIPE
        stderr = subprocess.PIPE

    proc = subprocess.Popen(
        cmd,
        shell=False,
        env=env,
        stdout=stdout,
        stderr=stderr,
        close_fds=True,
        preexec_fn=os.setsid,
    )
    return proc


def verify_install(install_script):
    if not install_script:
        # install_script not set means this "benchmark" does not need installation
        return True
    if os.path.exists("benchmark_installs.txt"):
        with open("benchmark_installs.txt", "r") as benchmark_installs:
            for benchmark_install in benchmark_installs:
                if install_script.strip() == benchmark_install.strip():
                    return True
    return False


def output_catcher(reader, writer=None):
    for line in iter(reader.readline, ""):
        if not line:
            continue
        if writer is not None:
            writer.write(line.rstrip() + "\n")
        click.echo(line.rstrip())


def install_benchmark(install_script, args=None, env=None, install_log=None):
    install_benchmark_cmd = ["bash", "-x", install_script]
    if args:
        install_benchmark_cmd.extend(args)
    install_benchmark_cmd = get_safe_cmd(install_benchmark_cmd)

    install_benchmark_proc = subprocess.Popen(
        install_benchmark_cmd,
        shell=False,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout_catcher = threading.Thread(
        target=output_catcher,
        name="stdout-catcher",
        args=(install_benchmark_proc.stdout, install_log),
    )
    stderr_catcher = threading.Thread(
        target=output_catcher,
        name="stderr-catcher",
        args=(install_benchmark_proc.stderr, install_log),
    )

    stdout_catcher.start()
    stderr_catcher.start()
    install_benchmark_proc.wait()
    stdout_catcher.join()
    stderr_catcher.join()

    if install_benchmark_proc.returncode == 0:
        verify_install_cmd_1 = ["touch", "benchmark_installs.txt"]
        verify_install_proc_1 = subprocess.Popen(verify_install_cmd_1, shell=False)
        verify_install_proc_1.wait()
        benchmark_installs_fp = open("benchmark_installs.txt", "a+")
        verify_install_cmd_2 = ["echo", install_script]
        verify_install_proc_2 = subprocess.Popen(
            verify_install_cmd_2, stdout=benchmark_installs_fp, shell=False
        )
        verify_install_proc_2.wait()
        benchmark_installs_fp.close()
    else:
        cmd_str = " ".join(install_benchmark_cmd)
        raise Exception(f"Failed to run '{cmd_str}'")

    return install_benchmark_proc


def install_tool(tool_name):
    install_script = "install_tool_" + tool_name + ".sh"
    if os.path.exists(install_script):
        if verify_install(install_script):
            return 0
        else:
            install_benchmark(install_script)
            return 1
    else:
        return -1  # install not needed


def create_benchmark_metrics_dir(run_id):
    benchmark_metrics_dir = "benchmark_metrics_{}".format(run_id)
    try:
        os.mkdir(benchmark_metrics_dir)
    except OSError as exc:
        if exc.errno != errno.EEXIST:
            raise
        pass
    return benchmark_metrics_dir


def clean_benchmark(clean_script, install_script):
    clean_benchmark_cmd = ["bash", "-x", clean_script]
    clean_benchmark_proc = subprocess.Popen(clean_benchmark_cmd, shell=False)
    clean_benchmark_proc.wait()
    if clean_benchmark_proc.returncode == 0:
        if os.path.exists("benchmark_installs.txt"):
            with open("benchmark_installs.txt", "r") as benchmark_installs_fp:
                lines = benchmark_installs_fp.readlines()
            with open("benchmark_installs.txt", "w") as benchmark_installs_fp:
                for line in lines:
                    if line.strip("\n") != install_script:
                        benchmark_installs_fp.write(line)
                benchmark_installs_fp.close()


def clean_tool(tool_name):
    clean_script = "clean_tool_" + tool_name + ".sh"
    if os.path.exists(clean_script):
        install_script = "install_tool_" + tool_name + ".sh"
        clean_benchmark(clean_script, install_script)
        return 1
    return -1


def kill_process(proc):
    proc.terminate()


def generate_timestamp():
    return int(time.time())


def generate_run_id():
    return str(uuid.uuid4())[-8:]


def initialize_env_vars(
    job,
    env: typing.Mapping[str, typing.Optional[str]],
    toolchain: str,
) -> typing.Dict[str, str]:
    """
    Populates the installer enviornment variables with Benchpress default vars.

    Arguments
        - job is a Benchpress job. Contains information about the benchmark to run
        - env is dictionary of environment variables to be set in the
                installer environment. These are typically inherited from the shell
                context executing benchpress.
        - toolchain is a choice among the options specified in job.toolchains

    Returns
        Dicitionary of environment variables
    """
    bp_env = {}
    if env is not None:
        bp_env.update(env)
    out_path = pathlib.Path(
        os.path.join(
            os.path.dirname(os.path.realpath(sys.argv[0])),
            BENCHMARKS_ROOT,
            job.benchmark_name,
        )
    )
    out_path.mkdir(mode=0o755, exist_ok=True, parents=True)
    tmp_abs_pathname = tempfile.mkdtemp(prefix=job.benchmark_name)
    bp_env["OUT"] = str(out_path)
    bp_env["BP_CPUS"] = str(multiprocessing.cpu_count())
    # Temporary directory for benchmark to download artifacts and carry out their build
    bp_env["BP_TMP"] = str(tmp_abs_pathname)

    # e.g.
    # clang:
    #   cc: /tmp/llvm/bin/clang
    #   cxx: /tmp/llvm/bin/clang++
    #   ldflags:
    #     - -fuse-ld=lld
    toolchain_config = job.toolchains[toolchain]
    for var in toolchain_config:
        bp_var = f"BP_{var.upper()}"
        value = toolchain_config[var]
        if isinstance(value, list):
            bp_env[bp_var] = " ".join(value)
        else:
            bp_env[bp_var] = value

    return bp_env
