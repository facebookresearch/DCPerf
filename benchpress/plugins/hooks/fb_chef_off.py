#!/usr/bin/env python3
# Copyright (c) 2021-present, Facebook, Inc.
# All rights reserved.

import logging
import subprocess

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

STOP_CHEF_PATH = "/usr/facebook/ops/scripts/chef/stop_chef_temporarily"
TURBODRIVER_PATH = "/usr/local/fbprojects/dynamoserver/bin/turboDriver"


class FBChefOff(Hook):
    """
    FBChefOff is a Facebook infrastructure specific hook that
    temporarily disables Chef
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
        proc = util.issue_background_command(chef_cmd, stdout=None, stderr=None)
        try:
            outs, errs = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            outs, errs = proc.communicate()
        logger.debug(outs)
        logger.debug(errs)

    def after_job(self, opts, job):
        pass
