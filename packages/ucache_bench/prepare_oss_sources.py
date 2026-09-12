# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from __future__ import annotations

import argparse
from pathlib import Path

_INTERNAL_PREFIX = "cea/chips/benchpress/packages/ucache_bench/protocol/gen/"
_INTERNAL_SERVER_PREFIX = "cea/chips/benchpress/packages/ucache_bench/server/"
_GENERATED_SUFFIXES: frozenset[str] = frozenset({".cpp", ".h", ".thrift"})


def _remove_internal_metadata_blocks(contents: str) -> str:
    lines = contents.splitlines(keepends=True)
    output: list[str] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.lstrip().startswith("if (FOLLY_UNLIKELY(request.get") and (
            "getPrivacyLibAgenticContext" in line or "getKcbIdentity" in line
        ):
            depth = line.count("{") - line.count("}")
            index += 1
            while index < len(lines) and depth > 0:
                depth += lines[index].count("{") - lines[index].count("}")
                index += 1
            continue
        output.append(line)
        index += 1
    return "".join(output)


def _make_oss_compatible(name: str, contents: str) -> str:
    contents = contents.replace(f'#include "{_INTERNAL_PREFIX}', '#include "')
    contents = contents.replace(
        f'#include "{_INTERNAL_SERVER_PREFIX}', '#include "../../server/'
    )
    contents = contents.replace(f'include "{_INTERNAL_PREFIX}', 'include "')
    contents = contents.replace(f'cpp_include "{_INTERNAL_PREFIX}', 'cpp_include "')

    if name == "UcacheBenchMessages.h" and "carbon/Fields.h" not in contents:
        contents = contents.replace(
            "#pragma once\n",
            "#pragma once\n\n#include <mcrouter/lib/carbon/Fields.h>\n",
            1,
        )
    if (
        name == "UcacheBenchRoutingGroups.h"
        and "carbon/RoutingGroups.h" not in contents
    ):
        contents = contents.replace(
            "#pragma once\n",
            "#pragma once\n\n#include <mcrouter/lib/carbon/RoutingGroups.h>\n",
            1,
        )
    if name == "UcacheBenchThriftTransport.h":
        contents = _remove_internal_metadata_blocks(contents)
    return contents


def prepare_sources(source_dir: Path, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    generated_files = sorted(
        path
        for path in source_dir.iterdir()
        if path.is_file() and path.suffix in _GENERATED_SUFFIXES
    )
    if not generated_files:
        raise ValueError(f"no generated sources found in {source_dir}")

    for source in generated_files:
        output = output_dir / source.name
        output.write_text(
            _make_oss_compatible(source.name, source.read_text(encoding="utf-8")),
            encoding="utf-8",
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Prepare generated Carbon sources for an out-of-tree OSS build"
    )
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    prepare_sources(args.source_dir, args.output_dir)


if __name__ == "__main__":
    main()
