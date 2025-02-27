#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from abc import ABCMeta, abstractmethod


class Hook(object, metaclass=ABCMeta):
    """Hook allows jobs to run some Python code before/after a job runs."""

    @abstractmethod
    def before_job(self, opts, job):
        """Do something to setup before this job.

        Args:
            opts (dict): user-defined options for this hook
        """

    @abstractmethod
    def after_job(self, opts, job):
        """Do something to teardown after this job.

        Args:
            opts (dict): user-defined options for this hook
        """
