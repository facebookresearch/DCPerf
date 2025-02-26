#!/usr/bin/env python3

import subprocess
import sys
import threading
import time


keys = ["user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal", "guest"]
cpu_util = {}


def get_cpu_ticks():
    with open("/proc/stat") as f:
        cpu = f.readline()
    curr_ticks = [int(x) for x in cpu.split()[1:]]
    return curr_ticks


def calc_cpu_util(end_ticks, start_ticks):
    diff_ticks = []
    cpu_util = {}
    for i in range(len(keys)):
        diff_ticks.append(end_ticks[i] - start_ticks[i])
    for category, tick in zip(keys, diff_ticks):
        cpu_util[category] = 100.0 * tick / sum(diff_ticks)
    return cpu_util


if __name__ == "__main__":
    start_ticks = get_cpu_ticks()
    p = subprocess.Popen(sys.argv[1:])
    ret = p.wait()
    end_ticks = get_cpu_ticks()
    cpu_util = calc_cpu_util(end_ticks, start_ticks)
    result = []
    for key in keys:
        result.append(f"{cpu_util[key]:.2f}")
    print(",".join(result))
    sys.exit(ret)
