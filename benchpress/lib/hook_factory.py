#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.plugins.hooks import register_hooks

from .factory import BaseFactory
from .hook import Hook

HookFactory = BaseFactory(Hook)

# register third-party hooks with the factory
register_hooks(HookFactory)
