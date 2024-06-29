#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from abc import ABCMeta, abstractmethod

# Defines how the table is styled
TABLE_FORMAT = "plain"


class BenchpressCommand(object, metaclass=ABCMeta):
    @abstractmethod
    def populate_parser(self, parser):
        pass

    @abstractmethod
    def run(self, args, jobs):
        pass
