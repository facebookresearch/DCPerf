#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import logging
import subprocess

from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)


class FBStopDynologd(Hook):
    """
    FBStopDynologd is a Facebook infrastructure specific hook that
    temporarily stops Dynolog. This is useful
    """

    def exec_cmd(self, cmd):
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            outs, errs = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            outs, errs = proc.communicate()
        logger.debug(outs)
        logger.debug(errs)

    def before_job(self, opts, job):
        """
        Stop dynologd service
        """
        cmd = [
            "/usr/bin/systemctl",
            "stop",
            "dynologd.service",
        ]
        logger.info("Stopping dynologd service")
        self.exec_cmd(cmd)

    def after_job(self, opts, job):
        """
        Start dynologd service after the job is done
        """
        cmd = [
            "/usr/bin/systemctl",
            "start",
            "dynologd.service",
        ]
        logger.info("Starting dynologd service")
        self.exec_cmd(cmd)
