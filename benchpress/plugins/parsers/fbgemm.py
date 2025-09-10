#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)


class FbgemmParser(Parser):
    def parse(self, stdout, stderr, returncode):
        # Patterns for the three formats
        cpu_a_pattern = re.compile(
            r"^(BLAS_FP32|FBP_t)\s+m\s*=\s*(\d+)\s+n\s*=\s*(\d+)\s+k\s*=\s*(\d+)\s+Gflops\s*=\s*([\d\.]+)\s+GBytes\s*=\s*([\d\.]+)"
        )
        embedding_result_pattern = re.compile(
            r"out type (\S+)\s+(\S+)\s+(cache (not )?flushed)\s+prefetch (on|off)\s+b/w\s+([\d\.eE\+\-]+) GB/s\s+effective b/w:\s+([\d\.eE\+\-]+)GB/s\s+time\s+([\d\.eE\+\-]+)\s+load_imbalance (\d+)"
        )
        batch_pattern = re.compile(
            r"batch size\s+(\d+)\s+num rows\s+(\d+)\s+emb dim\s+(\d+)\s+avg length\s+(\d+)"
        )
        index_pattern = re.compile(
            r"(\d+)\s+bit indices( with prefetching)?\, lengths_sum (\d+)"
        )

        metrics = {}
        embedding_config = {}
        embedding_metrics = []

        for line in stdout:
            line = line.strip()
            if not line:
                continue

            # Try EmbeddingSpMDM8BitBenchmark format
            m = batch_pattern.match(line)
            if m:
                embedding_config = {
                    "batch_size": int(m.group(1)),
                    "num_rows": int(m.group(2)),
                    "emb_dim": int(m.group(3)),
                    "avg_length": int(m.group(4)),
                }
                continue
            m = index_pattern.match(line)
            if m:
                embedding_config["index_bits"] = int(m.group(1))
                embedding_config["prefetching"] = bool(m.group(2))
                embedding_config["lengths_sum"] = int(m.group(3))
                continue
            m = embedding_result_pattern.match(line)
            if m:
                entry = embedding_config.copy()
                entry.update(
                    {
                        "out_type": m.group(1),
                        "op_type": m.group(2),
                        "cache_status": m.group(3),
                        "prefetch": m.group(5),
                        "b/w_GBps": float(m.group(6)),
                        "effective_b/w_GBps": float(m.group(7)),
                        "time": float(m.group(8)),
                        "load_imbalance": int(m.group(9)),
                    }
                )
                embedding_metrics.append(entry)
                continue

            # Try CPU_A format
            m = cpu_a_pattern.match(line)
            if m:
                type_, m_, n, k, gflops, gbytes = m.groups()
                key = f"CPU_A: {type_} m={m_} n={n} k={k}"
                metrics[key] = {"Gflops": float(gflops), "GBytes": float(gbytes)}
                continue

            # Try FbgemmParser format
            if "GOPS" in line:
                continue
            if "," in line:
                parameters = line.split(",")
                key = "GOPS: " + " ".join(p.strip() for p in parameters[:-1])
                try:
                    metrics[key] = float(parameters[-1].strip())
                except ValueError:
                    logger.warning(f"Could not parse metric value from line: {line}")
                continue

            logger.debug(f"Unrecognized line format: {line}")

        # Combine all metrics
        if embedding_metrics:
            metrics["embedding_spmdm8bit"] = embedding_metrics
        return metrics
