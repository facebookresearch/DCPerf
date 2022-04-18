#!/usr/bin/env python3
# Copyright (c) 2021-present, Facebook, Inc.
# All rights reserved.

import logging
import subprocess

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

TURBODRIVER_PATH = "/usr/local/fbprojects/dynamoserver/bin/turboDriver"


class FBTurboDriver(Hook):
    """
    FBTurboDriver is a Facebook infrastructure specific hook that enables or
    disables turbo with `turboDriver`. The only option is a string "enable" or
    "disable".
    """

    def before_job(self, opts, job):
        opt = str(opts)
        if opt not in {"enable", "disable"}:
            raise Exception(f"{opt} is not a valid option")

        turbodriver_cmd = [TURBODRIVER_PATH, opt]
        proc = util.issue_background_command(turbodriver_cmd, stdout=None, stderr=None)
        try:
            outs, errs = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            outs, errs = proc.communicate()
        logger.debug(outs)
        logger.debug(errs)

    def after_job(self, opts, job):
        pass
