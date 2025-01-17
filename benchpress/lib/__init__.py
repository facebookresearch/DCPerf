# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

try:
    # pyre-ignore[21]
    from benchpress import is_opensource  # @manual

    open_source = is_opensource
except ImportError:
    open_source = False
