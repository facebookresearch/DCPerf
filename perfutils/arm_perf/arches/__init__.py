#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""ARM CPU report modules.

Importing this package imports every per-CPU module, each of which calls
``core.register_arch`` at import time. To add a new ARM CPU, drop a new module
here and add it to the import list below -- no other file changes.
"""

try:
    from cea.chips.benchpress.perfutils.arm_perf.arches import (  # noqa: F401
        axion,
        grace,
        neoversev3,
    )
except ModuleNotFoundError:  # standalone / OSS: run from perfutils/ dir
    from arm_perf.arches import (  # noqa: F401  # pyre-ignore[21]
        axion,
        grace,
        neoversev3,
    )
