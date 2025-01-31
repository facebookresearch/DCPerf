#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from benchpress.lib.parser import Parser

#  Example output from Checkmark (-q, quiet mode)
#
# ./checkmark -q
# 1,60,0,4096,8388608,60.00,29745911,60919625728,36149.505,495765.175,1.215
#
# The fields are
#   1. threads
#   2. duration
#   3. thinktime
#   4. checksum_size
#   5. chunk_size
#   6. elapsed_time
#   7. chunk_ops
#   8. total_checksums
#   9. checksum_ms
#   10. chunk_rate
#   11. chunk_latency


class CheckmarkParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {
            "threads": 0,
            "duration": 0,
            "thinktime": 0,
            "checksum_size": 0,
            "chunk_size": 0,
            "elapsed_time": 0.0,
            "chunk_ops": 0,
            "total_checksums": 0,
            "checksum_ms": 0.0,
            "chunk_rate": 0.0,
            "chunk_latency": 0.0,
        }
        bench_out = stdout[0].strip().split(",")
        assert len(bench_out) == len(metrics)
        metrics["threads"] = int(bench_out[0])
        metrics["duration"] = int(bench_out[1])
        metrics["thinktime"] = int(bench_out[2])
        metrics["checksum_size"] = int(bench_out[3])
        metrics["chunk_size"] = int(bench_out[4])
        metrics["elapsed_time"] = float(bench_out[5])
        metrics["chunk_ops"] = int(bench_out[6])
        metrics["total_checksums"] = int(bench_out[7])
        metrics["checksum_ms"] = float(bench_out[8])
        metrics["chunk_rate"] = float(bench_out[9])
        metrics["chunk_latency"] = float(bench_out[10])
        return metrics
