#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import pathlib
import platform
import subprocess


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
        java_home = str(pathlib.Path(os.path.realpath(java_path)).parents[1])
    except subprocess.CalledProcessError:
        java_home = ""

    return java_home


if __name__ == "__main__":
    print(find_java_home())
