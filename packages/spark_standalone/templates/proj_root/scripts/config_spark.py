#!/usr/bin/env python3

import socket
from typing import Dict

from utils import read_sys_configs


PLATFORMS = []
ALL_HARDWARE_INFO = {}
STANDALONE_CONFIGS = {}
STANDALONE_CLI_ARGS = {}

sys_configs = read_sys_configs()
print(f"system cores={sys_configs['cores']} memory={sys_configs['memory']}")


def init_configs(aggressive=0):
    # derive a baseline default config
    if aggressive > 0:
        if aggressive > 1:
            spark_cores = int(sys_configs["cores"] / 3.3) * 4
        else:
            spark_cores = int(sys_configs["cores"] / 4) * 4
        spark_memory = int(sys_configs["memory"] - 7 - 5 - 6 - 2)
        executor_memory = int(spark_memory * 1024 * 4 / spark_cores * 0.9)
        spark_on_heap_memory = round(executor_memory * 0.6)
        spark_off_heap_memory = executor_memory - spark_on_heap_memory
    else:
        core_bound = sys_configs["memory"] / sys_configs["cores"] > 3.0
        spark_on_heap_memory = 4192 if core_bound else 2457
        spark_off_heap_memory = 2376 if core_bound else 2320
        executor_memory = spark_on_heap_memory + spark_off_heap_memory
        nominal_executor_memory = 10 if core_bound else 5.83
        accounted_executor_memory = (
            nominal_executor_memory + max(0.15 * nominal_executor_memory, 1)
        ) * 0.825
        nominal_spark_memory = int(sys_configs["memory"] - 7)
        spark_cores = 4 * min(
            int(sys_configs["cores"] / 3.3),
            int(nominal_spark_memory / accounted_executor_memory),
        )
        spark_memory = (spark_cores / 4 * executor_memory / 1024) + 1

    global PLATFORMS, ALL_HARDWARE_INFO, SPARK_CLI_ARGS, SPARK_CONFIGS
    PLATFORMS = [
        "default-default",
        # internal
        "bdw-2s",
        "skl-1s",
        "skl-2s",
        "cxl_skl-2s",
    ]

    ALL_HARDWARE_INFO = {
        "default-default": {
            "cores": sys_configs["cores"],
            "memory": spark_memory,
            "arch": sys_configs["arch"],
            "sockets": sys_configs["sockets"],
        },
        # internal
        "bdw-2s": {"cores": 56, "memory": 212},
        "skl-1s": {"cores": 36, "memory": 41},
        "skl-2s": {"cores": 80, "memory": 212},
        "cxl_skl-2s": {"cores": 80, "memory": 112},
    }

    STANDALONE_CONFIGS["default-default"] = {
        "spark.cores.max": spark_cores,
        "spark.default.parallelism": "16",
        "spark.driver.memory": "5g",
        "spark.driver.maxResultSize": "6g",
        "spark.executor.cores": "4",
        "spark.executor.memory": f"{spark_on_heap_memory}m",
        "spark.executor.extraJavaOptions": [
            "-Djava.net.preferIPv4Stack=false",
            "-Djava.net.preferIPv6Addresses=true",
            "-Dscala.usejavacp=true",
            "-XX:+PreserveFramePointer",
            "-XX:+UnlockDiagnosticVMOptions",
            "-XX:FreqInlineSize=128",
            "-XX:+UnlockExperimentalVMOptions",
            "-XX:ParallelGCThreads=4",
            "-XX:+UseParallelGC",
            "-XX:+UseParallelOldGC",
            "-XX:+PerfDisableSharedMem",
        ],
        "spark.memory.fraction": "0.2",
        "spark.memory.offHeap.enabled": "true",
        "spark.memory.offHeap.size": f"{spark_off_heap_memory}m",
        "spark.memory.useLegacyMode": "true",
        "spark.reducer.maxReqsInFlight": "32",
        "spark.shuffle.merge.enabled": "false",
        "spark.shuffle.service.enabled": "true",
        "spark.sql.crossJoin.enabled": "true",
    }
    for x in PLATFORMS:
        STANDALONE_CONFIGS[x] = STANDALONE_CONFIGS["default-default"].copy()

    # internal
    STANDALONE_CONFIGS["bdw-2s"].update(
        {
            "spark.cores.max": "64",
            "spark.executor.memory": "4192m",
            "spark.memory.offHeap.size": "3276m",
        }
    )
    STANDALONE_CONFIGS["skl-2s"].update(
        {
            "spark.cores.max": "92",
            "spark.executor.memory": "4192m",
            "spark.memory.offHeap.size": "3276m",
        }
    )
    STANDALONE_CONFIGS["skl-1s"].update(
        {
            "spark.cores.max": "36",
            "spark.executor.memory": "2457m",
            "spark.memory.offHeap.size": "1638m",
        }
    )
    STANDALONE_CONFIGS["cxl_skl-2s"].update(
        {
            "spark.cores.max": "24",
            "spark.executor.memory": "2457m",
            "spark.memory.offHeap.size": "1638m",
        }
    )

    STANDALONE_CLI_ARGS["default-default"] = {
        "--master": f"spark://{socket.gethostname()}:7379",
        "--driver-memory": STANDALONE_CONFIGS["default-default"]["spark.driver.memory"],
        "--executor-memory": STANDALONE_CONFIGS["default-default"][
            "spark.executor.memory"
        ],
    }
    for x in PLATFORMS:
        STANDALONE_CLI_ARGS[x] = STANDALONE_CLI_ARGS["default-default"].copy()
        STANDALONE_CLI_ARGS[x].update(
            {
                "--executor-memory": STANDALONE_CONFIGS[x]["spark.executor.memory"],
            }
        )


def check_platform(platform: str) -> None:
    if platform not in PLATFORMS:
        print(f"Unknown platform: {platform}")
        exit(1)


def get_hardware_info(platform: str) -> Dict:
    check_platform(platform)
    return ALL_HARDWARE_INFO[platform]


def get_standalone_cli_args(platform: str) -> Dict:
    check_platform(platform)
    return STANDALONE_CLI_ARGS[platform]


def get_standalone_configs(platform: str) -> Dict:
    check_platform(platform)
    return STANDALONE_CONFIGS[platform]
