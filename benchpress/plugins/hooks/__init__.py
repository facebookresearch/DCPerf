#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib import open_source

from .copy import CopyMoveHook
from .cpu_limit import CpuLimit
from .cpu_mpstat import CpuMpstat
from .emon import Emon
from .file import FileHook
from .perf import Perf
from .result import ResultHook
from .shell import ShellHook
from .tao_instruction import TaoInstructionHook
from .toplev import Toplev
from .user_script import UserScript

if not open_source:
    from .fb_chef_off import FBChefOff
    from .fb_chef_off_turbo_on import FBChefOffTurboOn
    from .fb_turbo_driver import FBTurboDriver


def register_hooks(factory):
    factory.register("copymove", CopyMoveHook)
    factory.register("cpu-limit", CpuLimit)
    factory.register("cpu-mpstat", CpuMpstat)
    factory.register("emon", Emon)
    factory.register("file", FileHook)
    factory.register("perf", Perf)
    factory.register("result", ResultHook)
    factory.register("shell", ShellHook)
    factory.register("tao_instruction", TaoInstructionHook)
    factory.register("toplev", Toplev)
    factory.register("user-script", UserScript)

    if not open_source:
        factory.register("fb_chef_off", FBChefOff)
        factory.register("fb_chef_off_turbo_on", FBChefOffTurboOn)
        factory.register("fb_turbo_driver", FBTurboDriver)
