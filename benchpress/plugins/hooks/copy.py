#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import glob
import logging
import os
import shutil
import sys

from benchpress.lib.hook import Hook

logger = logging.getLogger(__name__)


class CopyMoveHook(Hook):
    """CopyMoveHook provides the ability to copy or move certain files into
    the job's benchmark metrics folder before/after a job
    Options are a dictionary of 'before' and 'after' lists with a string for
    each paths to copy.
    """

    def __init__(self):
        # Path to directory of benchpress_cli.py
        self.basepath = os.path.dirname(os.path.abspath(sys.argv[0]))

    @staticmethod
    def do_copy_or_move(sources, dest, move=False):
        expanded_sources = []
        for src in sources:
            expanded_sources.extend(glob.glob(src))
        if not os.path.isdir(dest):
            os.mkdir(dest)
        for src in expanded_sources:
            if os.path.isfile(src) or os.path.islink(src):
                if move:
                    shutil.move(src, dest)
                else:
                    shutil.copy(src, dest)
            elif os.path.isdir(src):
                if move:
                    shutil.move(src, dest)
                else:
                    shutil.copytree(src, dest)
            else:
                logger.warning(f"Could not copy {src}.")

    def before_job(self, opts, job):
        destdir = self.basepath + "/benchmark_metrics_" + job.uuid
        is_move = True if "is_move" in opts and opts["is_move"] else False
        if "before" in opts:
            self.do_copy_or_move(opts["before"], destdir, is_move)

    def after_job(self, opts, job):
        destdir = self.basepath + "/benchmark_metrics_" + job.uuid
        is_move = True if "is_move" in opts and opts["is_move"] else False
        if "after" in opts:
            self.do_copy_or_move(opts["after"], destdir, is_move)
