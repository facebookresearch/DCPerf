# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from cea.chips.benchpress.packages.ucache_bench.prepare_oss_sources import (
    prepare_sources,
)


class PrepareOssSourcesTest(unittest.TestCase):
    def test_prepares_generated_sources_without_mutating_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "source"
            output = root / "output"
            source.mkdir()
            messages = source / "UcacheBenchMessages.h"
            messages.write_text(
                '#pragma once\n#include "cea/chips/benchpress/packages/'
                'ucache_bench/protocol/gen/UcacheBenchMessages-decl.h"\n'
            )
            service = source / "UcacheBenchService.thrift"
            service.write_text(
                'include "cea/chips/benchpress/packages/ucache_bench/'
                'protocol/gen/UcacheBench.thrift"\n'
            )
            transport = source / "UcacheBenchThriftTransport.h"
            transport.write_text(
                "if (FOLLY_UNLIKELY(request.getKcbIdentity().has_value())) {\n"
                "  send(request.getKcbIdentity().value());\n"
                "}\n"
            )

            prepare_sources(source, output)

            self.assertIn(
                "#include <mcrouter/lib/carbon/Fields.h>",
                (output / messages.name).read_text(),
            )
            self.assertIn(
                'include "UcacheBench.thrift"', (output / service.name).read_text()
            )
            self.assertNotIn("getKcbIdentity", (output / transport.name).read_text())
            self.assertIn("cea/chips/benchpress", messages.read_text())
