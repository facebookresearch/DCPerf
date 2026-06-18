#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Parser for `openssl speed` output as used by the dpu_perf suite.

Handles three distinct openssl-speed output shapes:

  1) Symmetric / hash table (`openssl speed -evp aes-256-gcm`):

       type             16 bytes     64 bytes    256 bytes   1024 bytes   8192 bytes  16384 bytes
       aes-256-gcm    1234567.89k  2345678.90k   3456789.01k   4567890.12k   5678901.23k  6789012.34k

     Throughput at each buffer size in 1000-byte/sec (the `k` is decimal
     kilobytes/sec per openssl convention, not 1024).

  2) Diffie-Hellman / signature ops table (`openssl speed ecdh`):

                                     op      op/s
        160 bits ecdh (secp160r1)   0.0001s   123456.7
        256 bits ecdh (nistp256)    0.0001s   123456.7

  3) Sign/verify ops table (`openssl speed ecdsa rsa`):

                                     sign    verify    sign/s verify/s
        160 bits ecdsa (secp160r1)   0.0001s   0.0002s   12345.6    9876.5

We emit:
  - cipher_throughput_MBps_at_<N>B  (for each block-size column)
  - cipher_throughput_MBps_max      (max across block sizes)
  - cipher_algo                     (the EVP algo name)
  - asym_ops: list of [algo, op_type, ops_per_sec]
  - asym_best_ops_per_sec / asym_best_algo
"""

import re
from typing import Iterable, List

from benchpress.lib.parser import Parser


# Cipher/hash header looks like: "type 16 bytes 64 bytes 256 bytes ..."
_CIPHER_HEADER_RE = re.compile(r"^\s*type\b")
# Per-row: "<algo>  <val>k  <val>k  ...". The `k` suffix is openssl convention
# for 1000-byte/sec (decimal). We capture algo + every "<num>k" token.
_CIPHER_K_RE = re.compile(r"([0-9]+\.[0-9]+|[0-9]+)k\b")

# Header that introduces a "op / op/s" or "sign/verify" asym table.
_ASYM_HEADER_RE = re.compile(r"^\s+(op\b|sign\b)")
# Per-row: leading algo name, then time/sec pairs ending in numeric op/s.
# We grab the last float as op/s (or two trailing floats for sign+verify).


def _read_cipher_block(lines: Iterable[str]) -> dict:
    """Parse the symmetric/hash table starting from the header line."""
    out = {}
    header = None
    bytes_columns: List[int] = []
    for raw in lines:
        line = raw.rstrip()
        if not line.strip():
            continue
        if _CIPHER_HEADER_RE.match(line):
            header = line
            # Pull every "<N> bytes" into bytes_columns.
            bytes_columns = [int(b) for b in re.findall(r"(\d+)\s*bytes", header)]
            continue
        if header is None:
            continue
        # First token is the algo name; subsequent tokens are <num>k values.
        parts = line.strip().split()
        if not parts:
            continue
        algo = parts[0]
        # The "k" suffix is openssl convention for 1000 B/s.
        rates_kBps = [float(m) for m in _CIPHER_K_RE.findall(line)]
        if not rates_kBps:
            continue
        out["cipher_algo"] = algo
        rates_MBps = [r * 1000.0 / 1_000_000.0 for r in rates_kBps]  # → MB/s decimal
        for i, mbps in enumerate(rates_MBps):
            block = bytes_columns[i] if i < len(bytes_columns) else i
            # pyrefly: ignore [unsupported-operation]
            out[f"cipher_throughput_MBps_at_{block}B"] = mbps
        # pyrefly: ignore [unsupported-operation]
        out["cipher_throughput_MBps_max"] = max(rates_MBps)
        return out
    return out


# Trailing time-per-op token, e.g. "0.0000s" or "0s".
_TIME_TOK_RE = re.compile(r"^\d+(?:\.\d+)?s$")
# Pure float / int (no 's' suffix) — op/s column tokens.
_FLOAT_TOK_RE = re.compile(r"^\d+(?:\.\d+)?$")


def _read_asym_block(lines: Iterable[str]) -> dict:
    """Parse the asym (ecdh / ecdsa / rsa / dh) op/s tables.

    Sample row (ECDH): "256 bits ecdh (nistp256)   0.0001s  24468.7"
    Sample row (ECDSA sign+verify):
        "256 bits ecdsa (nistp256)   0.0001s   0.0002s   12345.6    9876.5"

    We split on whitespace, then walk the token list to find the first
    time-format token ("Ns" / "N.NNNNs"). Algo label = everything before
    that; ops/s columns = pure floats after the time tokens.
    """
    out: dict = {}
    asym_rows: List[list] = []
    in_block = False
    for raw in lines:
        line = raw.rstrip()
        if not line.strip():
            in_block = False
            continue
        if _ASYM_HEADER_RE.match(line):
            in_block = True
            continue
        if not in_block:
            continue
        toks = line.strip().split()
        # Find the first time-format token; everything before it is the algo
        # label.
        first_time_idx = next(
            (i for i, t in enumerate(toks) if _TIME_TOK_RE.match(t)),
            None,
        )
        if first_time_idx is None or first_time_idx == 0:
            continue
        algo = " ".join(toks[:first_time_idx]).strip()
        # After the time column(s), the trailing pure-float tokens are op/s.
        ops_per_sec = [
            float(t) for t in toks[first_time_idx:] if _FLOAT_TOK_RE.match(t)
        ]
        if not ops_per_sec:
            continue
        if len(ops_per_sec) == 1:
            asym_rows.append([algo, "op", ops_per_sec[0]])
        else:
            # First is sign/s, second is verify/s (any extras are ignored).
            asym_rows.append([algo, "sign", ops_per_sec[0]])
            asym_rows.append([algo, "verify", ops_per_sec[1]])

    if asym_rows:
        out["asym_ops"] = asym_rows
        best = max(asym_rows, key=lambda r: r[2])
        out["asym_best_algo"] = best[0]
        out["asym_best_op"] = best[1]
        out["asym_best_ops_per_sec"] = best[2]
    return out


class OpensslSpeedParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        # Materialise so we can scan twice (cipher block, asym block).
        lines = list(stdout)
        metrics.update(_read_cipher_block(lines))
        metrics.update(_read_asym_block(lines))
        return metrics
