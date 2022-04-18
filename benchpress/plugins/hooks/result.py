#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import logging
import os
import shutil

from benchpress.lib import util
from benchpress.lib.hook import Hook

logger = logging.getLogger(__name__)


class ResultHook(Hook):
    """ResultHook provides the ability to save files into metric folder.
    Job name will be added as a prefix.
    Options are specified as a list of file paths.
    """

    def before_job(self, opts, job):
        self.benchmark_metrics_dir = util.create_benchmark_metrics_dir(job.uuid)

    def after_job(self, opts, job):
        for opt in opts:
            if os.path.exists(opt):
                filename = f"{job.name}_{os.path.basename(opt)}"
                dst = os.path.join(self.benchmark_metrics_dir, filename)
                shutil.copyfile(opt, dst)
