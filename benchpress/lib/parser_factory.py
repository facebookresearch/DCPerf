#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from benchpress.plugins.parsers import register_parsers

from .factory import BaseFactory
from .parser import Parser

ParserFactory = BaseFactory(Parser)

# register third-party parsers with the factory
register_parsers(ParserFactory)
