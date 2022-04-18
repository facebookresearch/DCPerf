#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import logging
import subprocess

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

STOP_CHEF_PATH = "/usr/facebook/ops/scripts/chef/stop_chef_temporarily"
TURBODRIVER_PATH = "/usr/local/fbprojects/dynamoserver/bin/turboDriver"


class FBChefOffTurboOn(Hook):
    """
    FBChefOffTurboOn is a Facebook infrastructure specific hook that
    temporarily disables Chef, and enables turbo ON with `turboDriver`
    """

    def before_job(self, opts, job):
        """
        Ignores opts. Runs in background.
        """
        chef_cmd = [
            STOP_CHEF_PATH,
            "-r",
            "'benchpress(oncall: chips_tools)'",
            "-t",
            "24",
        ]
        turbodriver_cmd = [TURBODRIVER_PATH, "enable"]

        cmds = [chef_cmd, turbodriver_cmd]
        for cmd in cmds:
            proc = util.issue_background_command(cmd, stdout=None, stderr=None)
            try:
                outs, errs = proc.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                outs, errs = proc.communicate()
            logger.debug(outs)
            logger.debug(errs)

    def after_job(self, opts, job):
        pass
