#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from benchpress.plugins.hooks import register_hooks

from .factory import BaseFactory
from .hook import Hook

HookFactory = BaseFactory(Hook)

# register third-party hooks with the factory
register_hooks(HookFactory)
