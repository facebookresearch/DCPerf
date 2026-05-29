#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for `lzbench` standard (raw) output as used by the dpu_perf suite.

lzbench prints a header + per-codec rows:

    lzbench 1.8.1 (64-bit Linux)   Assembled by P.Skibinski
    Compressor name          Compress. Decompress. Compr. size  Ratio Filename
    memcpy                    8889 MB/s   8930 MB/s   211957760 100.00 silesia.tar
    zstd 1.5.4 -1              450 MB/s    1300 MB/s    72445832  34.18 silesia.tar
    zstd 1.5.4 -3              340 MB/s    1280 MB/s    68000000  32.08 silesia.tar

We use a single regex anchored on the last 4 numeric fields so the variable
"Compressor name" column (which may contain spaces, version tags, and a
"-N" level suffix) can be captured as the leading slice.

Emitted metrics:
  - codecs: list of [name, compress_MBps, decompress_MBps, comp_size, ratio_pct]
  - best_compress_MBps / best_decompress_MBps (excluding the memcpy reference
    row, which represents the raw IO ceiling, not compression)
  - best_compress_codec / best_decompress_codec
  - memcpy_compress_MBps / memcpy_decompress_MBps (if present)
"""

import re
from typing import List, Optional, Tuple

from benchpress.lib.parser import Parser


# Capture: leading-name, comp_MBps, decomp_MBps, comp_size, ratio
_ROW_RE = re.compile(
    r"^(?P<name>\S.*?)\s+"
    r"(?P<comp>\d+(?:\.\d+)?)\s*MB/s\s+"
    r"(?P<decomp>\d+(?:\.\d+)?)\s*MB/s\s+"
    r"(?P<size>\d+)\s+"
    r"(?P<ratio>\d+\.\d+)"
)


def _parse_row(line: str) -> Optional[Tuple[str, float, float, int, float]]:
    m = _ROW_RE.match(line.rstrip())
    if not m:
        return None
    return (
        m.group("name").strip(),
        float(m.group("comp")),
        float(m.group("decomp")),
        int(m.group("size")),
        float(m.group("ratio")),
    )


class LzbenchPerfParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        codecs: List[list] = []
        for raw in stdout:
            parsed = _parse_row(raw)
            if parsed is None:
                continue
            name, comp, decomp, size, ratio = parsed
            codecs.append([name, comp, decomp, size, ratio])

        if not codecs:
            return metrics

        metrics["codecs"] = codecs
        metrics["num_codecs"] = len(codecs)

        # memcpy is lzbench's reference row — captures raw IO ceiling. Record
        # separately and exclude from best-compression aggregation.
        non_memcpy = []
        for row in codecs:
            if row[0].lower().startswith("memcpy"):
                metrics["memcpy_compress_MBps"] = row[1]
                metrics["memcpy_decompress_MBps"] = row[2]
            else:
                non_memcpy.append(row)

        if non_memcpy:
            best_comp = max(non_memcpy, key=lambda r: r[1])
            best_dec = max(non_memcpy, key=lambda r: r[2])
            metrics["best_compress_codec"] = best_comp[0]
            metrics["best_compress_MBps"] = best_comp[1]
            metrics["best_decompress_codec"] = best_dec[0]
            metrics["best_decompress_MBps"] = best_dec[2]
        return metrics
