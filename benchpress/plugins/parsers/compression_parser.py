#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)

REGEX_CORPUS_NAME = r"silesia\/(\w+\-?\w+)"
REGEX_VAL_BEFORE_MBS = r" (\d+\.?\d+?) MB\/s"


class CompressionParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        level_comp_speed = {}
        level_decomp_speed = {}
        compression_level_pool = [1, 4, 8, 16, 19]
        idx = 0
        level = 0
        corpus_name = ""
        new_corpus_name = "some"
        stdall = stdout + stderr

        # count corpus
        count = 0
        for line in stdall:
            if (
                re.search("silesia/", line)
                and not re.search("memcpy", line)
                and not re.search("overwrite", line)
            ):
                new_corpus_name = re.findall(REGEX_CORPUS_NAME, line)[0]
                if corpus_name != new_corpus_name:
                    level = 0
                    corpus_name = new_corpus_name
                    count = count + 1
                level = compression_level_pool[idx]
                idx = (idx + 1) % len(compression_level_pool)
                speed = re.findall(REGEX_VAL_BEFORE_MBS, line)
                if len(speed) != 0:
                    if level not in level_comp_speed:
                        level_comp_speed[level] = 1
                    level_comp_speed[level] = level_comp_speed[level] * float(speed[0])
                    if level not in level_decomp_speed:
                        level_decomp_speed[level] = 1
                    level_decomp_speed[level] = level_decomp_speed[level] * float(
                        speed[1]
                    )
        # the count counts the number of copus, due to regex dulicate in stdout
        # count doubles, hence count/2 is the actual corpus count
        count = count / 2
        for k, v in level_comp_speed.items():
            metrics["geo mean comp speed (MB/s) level " + str(k)] = v ** (1 / count)
        for k, v in level_decomp_speed.items():
            metrics["geo mean decomp speed (MB/s) level " + str(k)] = v ** (1 / count)
        return metrics
