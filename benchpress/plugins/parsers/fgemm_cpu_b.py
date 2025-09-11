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


class FbgemmParser_CPU_B(Parser):
    def parse(self, stdout, stderr=None, returncode=None):
        metrics = []
        context = {}
        # Patterns for context and metric lines
        batch_pattern = re.compile(
            r"batch size\s+(\d+)\s+num rows\s+(\d+)\s+emb dim\s+(\d+)\s+avg length\s+(\d+)"
        )
        index_pattern = re.compile(
            r"(64|32) bit indices( with prefetching)?\, lengths_sum (\d+)"
        )
        metric_pattern = re.compile(
            r"out type fp32\s+(SLS|SLW\(WEIGHTED\))\s+(cache (not )?flushed)\s+prefetch (on|off)\s+b/w\s+([\d\.]+) GB/s\s+effective b/w:\s+([\d\.]+)GB/s\s+time\s+([\deE\.\-]+)\s+load_imbalance (\d+)"
        )

        for line in stdout:
            line = line.strip()
            if not line:
                continue

            # Parse batch context
            batch_match = batch_pattern.match(line)
            if batch_match:
                context["batch_size"] = int(batch_match.group(1))
                context["num_rows"] = int(batch_match.group(2))
                context["emb_dim"] = int(batch_match.group(3))
                context["avg_length"] = int(batch_match.group(4))
                continue

            # Parse index context
            index_match = index_pattern.match(line)
            if index_match:
                context["index_bits"] = int(index_match.group(1))
                context["prefetching"] = bool(index_match.group(2))
                context["lengths_sum"] = int(index_match.group(3))
                continue

            # Parse metrics
            metric_match = metric_pattern.match(line)
            if metric_match:
                metric = {
                    "out_type": "fp32",
                    "op_type": metric_match.group(1),
                    "cache_status": metric_match.group(3),
                    "prefetch": metric_match.group(4),
                    "b_w": float(metric_match.group(5)),
                    "effective_b_w": float(metric_match.group(6)),
                    "time": float(metric_match.group(7)),
                    "load_imbalance": int(metric_match.group(8)),
                    # Copy current context
                    "batch_size": context.get("batch_size"),
                    "num_rows": context.get("num_rows"),
                    "emb_dim": context.get("emb_dim"),
                    "avg_length": context.get("avg_length"),
                    "index_bits": context.get("index_bits"),
                    "prefetching": context.get("prefetching"),
                    "lengths_sum": context.get("lengths_sum"),
                }
                metrics.append(metric)
        return metrics
