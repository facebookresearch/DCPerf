#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from .cpu_limit import CpuLimit
from .cpu_mpstat import CpuMpstat
from .emon import Emon
from .fb_chef_off import FBChefOff
from .fb_chef_off_turbo_on import FBChefOffTurboOn
from .fb_turbo_driver import FBTurboDriver
from .file import FileHook
from .perf import Perf
from .result import ResultHook
from .shell import ShellHook
from .toplev import Toplev
from .user_script import UserScript


def register_hooks(factory):
    factory.register("cpu-limit", CpuLimit)
    factory.register("cpu-mpstat", CpuMpstat)
    factory.register("emon", Emon)
    factory.register("fb_chef_off", FBChefOff)
    factory.register("fb_chef_off_turbo_on", FBChefOffTurboOn)
    factory.register("fb_turbo_driver", FBTurboDriver)
    factory.register("file", FileHook)
    factory.register("perf", Perf)
    factory.register("result", ResultHook)
    factory.register("shell", ShellHook)
    factory.register("toplev", Toplev)
    factory.register("user-script", UserScript)
