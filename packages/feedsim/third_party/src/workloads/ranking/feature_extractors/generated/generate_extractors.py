#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Generate diverse feature extractor copy functions with 27 code structure patterns.

Produces ~700 extractor variants across 27 genuinely different code structure
patterns, each with 1000 copies (default). Each pattern has different branch
topology, loop structure, data access patterns, and code size to create
realistic I-cache pressure matching production feature extractors.

Output:
    dispatch.h                       -- CopyContext struct, CopyFn typedef
    copies/copies_part_NNNN.cpp      -- 1000 copy functions per variant
    variants/variants_batch_NNN.cpp  -- variant class definitions (dispatch)
    registry.h / registry.cpp        -- createGeneratedExtractors()
    generated_sources.cmake          -- CMake include fragment
"""

import argparse
import os
import random
import sys
import time
from dataclasses import dataclass
from typing import List

RANDOM_SEED = 424242

COPYRIGHT = """\
// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
"""

# Pattern distribution: (pattern_id, variant_count, struct_size, table_size)
PATTERN_DISTRIBUTION = [
    ("P01", 60, 64, 0),        # Minimal field read
    ("P02", 70, 128, 0),       # Multi-field read
    ("P03", 30, 128, 0),       # Multi-field + enum dispatch
    ("P04", 15, 64, 0),        # Time arithmetic
    ("P05", 40, 256, 256),     # Simple rate/counter
    ("P06", 65, 512, 256),     # Dimensional rate
    ("P07", 25, 512, 512),     # Multi-conditional rate
    ("P08", 45, 128, 2048),    # Single hash lookup + rates
    ("P09", 25, 256, 4096),    # Multi-key hash + dedup
    ("P10", 15, 256, 2048),    # Dual hash lookup
    ("P11", 8, 512, 2048),     # Hash + tree traversal
    ("P12", 130, 256, 0),      # Template time-window stats
    ("P13", 5, 1024, 512),     # Massive coefficient yield
    ("P14", 12, 512, 512),     # Decayed coefficient
    ("P15", 20, 256, 1024),    # Embedding dimension yield
    ("P16", 15, 512, 0),       # Embedding similarity
    ("P17", 15, 128, 0),       # Sparse feature forward
    ("P18", 8, 128, 1024),     # Hash-then-sparse forward
    ("P19", 20, 128, 0),       # LastN ID match
    ("P20", 15, 128, 0),       # Set membership match
    ("P21", 3, 2048, 4096),    # Large monolithic prediction
    ("P22", 20, 512, 1024),    # Medium prediction forward
    ("P23", 8, 512, 0),        # SentencePiece tokenization
    ("P24", 6, 256, 0),        # Mutable state read/update
    ("P25", 15, 256, 2048),    # Multi-topic hash iteration
    ("P26", 4, 64, 0),         # Delegated dynamic feature generation
    ("P27", 5, 256, 2048),     # Multi-category hierarchical FIH
]

TRANSFORMS = [
    "r",
    "static_cast<float>(static_cast<int32_t>(r * 1000.0f) & 0x7FFFFFFF)",
    "static_cast<float>(__builtin_popcount(static_cast<uint32_t>(r * 1e6f)))",
    "static_cast<float>((static_cast<int64_t>(r * 1e4f) ^ 0x5BD1E995LL) >> 13)",
    "(r > 0.5f ? r : -r)",
    "static_cast<float>((static_cast<uint64_t>(r * 1e6f) * 0x9E3779B97F4A7C15ULL) >> 48)",
    "static_cast<float>(static_cast<int32_t>(r) << 3)",
    "static_cast<float>((static_cast<int32_t>(r * 256.0f) ^ (static_cast<int32_t>(r * 65536.0f) >> 7)) & 0xFFFF)",
]


@dataclass
class VariantSpec:
    name: str
    pattern: str
    variant_idx: int
    pattern_variant_idx: int
    seed: int
    struct_size: int
    table_size: int


# ============================================================================
# C++ code generation helpers
# ============================================================================

def fhdr(vid, cid):
    """Function header with context preparation call and validity pre-check."""
    seed = vid * 1000 + cid
    guard_off = (seed * 7 + 13) % 256
    guard_thr = (seed % 16)
    return (f"__attribute__((noinline)) static void "
            f"vc_{vid:04d}_{cid:04d}(CopyContext* c) {{\n"
            f"  using namespace dcperf::feature_extractors;\n"
            f"  helpers::prepareExtractContext(c, {seed});\n"
            f"  if ((static_cast<int>(c->structData[{guard_off} % c->structSize]) & 0xF) < {guard_thr}) {{\n"
            f"    c->structData[0] += 1e-10f;\n"
            f"    return;\n"
            f"  }}\n")


def sr(off):
    """Struct data read."""
    return f"c->structData[{off} % c->structSize]"


def yd(fi, val):
    """Yield dense feature — INLINE unique code (I-cache pressure) + helper validation."""
    # Keep the original inline code for unique instruction footprint,
    # AND call helper for additional non-inline hops.
    return (f"    {{ int _fi = {fi} % c->numFeatures;\n"
            f"      int32_t _i = c->features[_fi].raw_feature_index;\n"
            f"      if (_i >= 0 && _i < (int32_t)c->example->denseValues.size()) {{\n"
            f"        float _v = {val};\n"
            f"        if (dcperf::feature_extractors::helpers::validateFeatureValue(_v, {fi % 3}))\n"
            f"          c->example->denseValues[_i] = _v;\n"
            f"      }} }}\n")


def yi(fi, idx, val):
    """Yield indexed feature — INLINE unique code + helper validation."""
    return (f"    {{ int _fi = {fi} % c->numFeatures;\n"
            f"      float _v = {val};\n"
            f"      if (_fi >= 0 && _fi < (int)c->example->idScoreLists.size()\n"
            f"          && dcperf::feature_extractors::helpers::validateFeatureValue(_v, {fi % 3}))\n"
            f"        c->example->idScoreLists[_fi].emplace_back(\n"
            f"          IdScorePair{{{idx}, _v}}); }}\n")


def hl(table_idx, key_expr, fallback="0.0f"):
    """Hash lookup via mock hash table (adds 5-7 non-inline hops)."""
    return (f"dcperf::mock_hash::hashLookupWithFallback("
            f"c->hashTables[{table_idx} % c->numHashTables], "
            f"static_cast<int64_t>({key_expr}), {fallback})")


def hj(tA, tB, key_expr, scale="1000.0f"):
    """Cross-table hash join (adds 7-10 non-inline hops)."""
    return (f"dcperf::mock_hash::crossTableLookup("
            f"c->hashTables[{tA} % c->numHashTables], "
            f"c->hashTables[{tB} % c->numHashTables], "
            f"static_cast<int64_t>({key_expr}), {scale})")


def ls(table_idx, stat_type):
    """Lookup stats via helper (adds 6-8 non-inline hops)."""
    return (f"dcperf::feature_extractors::helpers::lookupStats("
            f"c->hashTables[{table_idx} % c->numHashTables], "
            f"c->queryKeys, c->numKeys, {stat_type})")


def cr(num, den, bucket_type):
    """Compute rate via helper (adds 3-4 non-inline hops)."""
    return (f"dcperf::feature_extractors::helpers::computeRate("
            f"{num}, {den}, {bucket_type})")


def pick_transform(rng):
    return rng.choice(TRANSFORMS)


def int_hash_expr(rng, input_var):
    """Generate a unique integer hash computation that replaces FP math."""
    magic1 = hex(rng.randint(0, 0xFFFFFFFF))
    magic2 = hex(rng.randint(0, 0xFFFFFFFF))
    shift = rng.randint(7, 19)
    return (f"static_cast<float>((static_cast<uint32_t>({input_var} * 1e4f) "
            f"* {magic1}u ^ {magic2}u) >> {shift})")


def hash_block(rng, input_off, num_ops=20, num_guards=0):
    """Generate a block of integer hash computations to increase BB size.

    When num_guards > 0, interleaves rarely-taken guard branches throughout
    the computation.  These branches count as conditional-branch instructions
    (increasing br_inst_retired.cond) but are almost never taken (~1/256),
    so they do NOT split LBR-measured basic blocks — matching production's
    pattern of many not-taken data-dependent checks within straight-line code.
    """
    c = ""
    c += f"  uint64_t _h = static_cast<uint64_t>(c->structData[{input_off} % c->structSize] * 1e6f);\n"
    guard_interval = max(1, num_ops // (num_guards + 1)) if num_guards > 0 else num_ops + 1
    guard_idx = 0
    for i in range(num_ops):
        magic = hex(rng.randint(0x10000000, 0xFFFFFFFFFFFFFFFF))
        shift = rng.randint(11, 23)
        next_off = (input_off + i + 1) % 512
        op = rng.choice([
            f"  _h = _h * {magic}ULL; _h ^= _h >> {shift};\n",
            f"  _h += static_cast<uint64_t>(c->structData[{next_off} % c->structSize] * 1e4f); _h *= {magic}ULL;\n",
            f"  _h ^= _h >> {shift}; _h *= {magic}ULL; _h ^= _h >> {shift + 3};\n",
        ])
        c += op
        # Insert rarely-taken guard branch every guard_interval ops
        if num_guards > 0 and (i + 1) % guard_interval == 0 and guard_idx < num_guards:
            rare_val = hex(rng.randint(0x80, 0xFF))
            sd_off = (input_off + guard_idx) % 256
            c += (f"  if ((_h & 0xFF) == {rare_val}) "
                  f"c->structData[{sd_off} % c->structSize] += 1e-15f;\n")
            guard_idx += 1
    c += f"  float _hf = static_cast<float>(_h >> 32) * 1e-9f;\n"
    return c


def guard_cascade(rng, num_guards=12):
    """Generate a cascade of rarely-taken guard branches with computation.

    Each guard adds a conditional branch instruction (~1/256 taken) plus
    2-3 hash ops between guards.  Combined with hash_block, this raises
    conditional-branch % toward production's 14% without splitting LBR BBs.
    Requires _h and _hf to already be declared (call after hash_block).
    """
    c = ""
    for g in range(num_guards):
        # 2-3 hash ops between guards (keeps _h changing so guards are independent)
        for _ in range(rng.randint(2, 3)):
            magic = hex(rng.randint(0x10000000, 0xFFFFFFFFFFFFFFFF))
            shift = rng.randint(7, 23)
            c += f"  _h = _h * {magic}ULL; _h ^= _h >> {shift};\n"
        # Guard branch: true ~1/256 of the time
        rare_val = hex(rng.randint(0x00, 0xFF))
        sd_off = rng.randint(0, 255)
        c += (f"  if ((_h & 0xFF) == {rare_val}) "
              f"c->structData[{sd_off} % c->structSize] += 1e-15f;\n")
    c += f"  _hf += static_cast<float>(_h >> 48) * 1e-12f;\n"
    return c


# ============================================================================
# Pattern P01: Minimal field read (0 loops, 1-3 branches, 1-3 yields)
# ~12-20 lines per copy
# ============================================================================

def gen_P01(vid, cid, rng, pvid):
    num_yields = 1 + (pvid % 3)
    has_null = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    # Hash block with guards to increase BB size and conditional branch %
    c += hash_block(rng, rng.randint(0, 255), num_ops=50)
    off0 = rng.randint(0, 255)
    sc0 = rng.uniform(0.5, 3.0)
    if has_null:
        c += f"  float v0 = {sr(off0)} * {sc0:.4f}f + _hf;\n"
        c += f"  if (v0 == 0.0f) return;\n"
    for i in range(num_yields):
        off = rng.randint(0, 255)
        fi = rng.randint(0, 49)
        sc = rng.uniform(0.3, 4.0)
        c += f"  {{ float val = {sr(off)} * {sc:.4f}f;\n"
        # Feature validity check branch
        c += f"    if (c->features[{fi} % c->numFeatures].raw_feature_index >= 0) {{\n"
        c += yd(fi, "val")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P02: Multi-field read (0 loops, 3-8 branches, 3-10 yields)
# ~25-50 lines per copy
# ============================================================================

def gen_P02(vid, cid, rng, pvid):
    num_fields = 3 + (pvid % 8)  # 3-10
    num_branches = 3 + (pvid % 6)  # 3-8
    has_cross = (pvid % 3) == 0
    c = fhdr(vid, cid)
    # Hash block with guards to increase BB size and conditional branch %
    c += hash_block(rng, rng.randint(0, 255), num_ops=35)
    # Read from 1-2 "sources" (different struct offset regions)
    src_base = [rng.randint(0, 127), rng.randint(128, 255)]
    for i in range(num_fields):
        src = src_base[i % 2]
        off = src + rng.randint(0, 60)
        fi = rng.randint(0, 49)
        sc = rng.uniform(0.1, 5.0)
        thr = rng.uniform(-1.0, 1.0)
        if i < num_branches:
            check = rng.choice([
                f"{sr(off)} > {thr:.4f}f",
                f"{sr(off)} != 0.0f",
                f"std::abs({sr(off)}) > {abs(thr):.4f}f",
            ])
            c += f"  if ({check}) {{\n"
            c += f"    float val = {sr(off)} * {sc:.4f}f;\n"
            c += yd(fi, "val")
            c += f"  }}\n"
        else:
            c += f"  {{ float val = {sr(off)} * {sc:.4f}f;\n"
            c += yd(fi, "val")
            c += f"  }}\n"
    if has_cross:
        oA = rng.randint(0, 127)
        oB = rng.randint(128, 255)
        fi = rng.randint(0, 49)
        c += f"  {{ float a = {sr(oA)}; float b = {sr(oB)};\n"
        c += f"    if (b != 0.0f) {{\n"
        # Integer bit manipulation for reciprocal approximation
        c += f"      int32_t _den_bits = *reinterpret_cast<int32_t*>(&b);\n"
        c += f"      _den_bits = 0x7EF311C2 - _den_bits;\n"
        c += f"      float _inv_b = *reinterpret_cast<float*>(&_den_bits);\n"
        c += f"      float cross = a * _inv_b + _hf;\n"
        c += yd(fi, "cross")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P03: Multi-field + enum dispatch (switch, 3-10 cases)
# ~35-65 lines per copy
# ============================================================================

def gen_P03(vid, cid, rng, pvid):
    num_cases = 3 + (pvid % 8)  # 3-10
    yields_per_case = 1 + (pvid % 3)
    has_default = (pvid + cid) % 2 == 0
    type_off = rng.randint(0, 255)
    c = fhdr(vid, cid)
    c += hash_block(rng, rng.randint(0, 127), num_ops=30)
    c += f"  int type_val = static_cast<int>({sr(type_off)}) & 0xF;\n"
    c += f"  switch (type_val) {{\n"
    for case_i in range(num_cases):
        c += f"    case {case_i}: {{\n"
        for y in range(yields_per_case):
            off = rng.randint(0, 255)
            fi = rng.randint(0, 49)
            sc = rng.uniform(0.5, 3.0)
            if y % 2 == 0:
                c += f"      {{ float v = {sr(off)} * {sc:.4f}f;\n"
                c += f"  " + yd(fi, "v")
                c += f"      }}\n"
            else:
                idx_off = rng.randint(0, 255)
                c += f"      {{ float v = {sr(off)} * {sc:.4f}f;\n"
                c += f"  " + yi(fi, f"static_cast<int64_t>({sr(idx_off)})", "v")
                c += f"      }}\n"
        c += f"      break;\n"
        c += f"    }}\n"
    if has_default:
        fi = rng.randint(0, 49)
        c += f"    default:\n"
        c += f"  " + yd(fi, "1.0f")
        c += f"      break;\n"
    c += f"  }}\n"
    # Extra conditional after switch
    extra_off = rng.randint(0, 255)
    fi = rng.randint(0, 49)
    c += f"  {{ float extra = {sr(extra_off)};\n"
    c += f"    if (extra != 0.0f) {{\n"
    c += yd(fi, "extra")
    c += f"    }}\n"
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P04: Time arithmetic (3-5 branches, no loops, bucketing)
# ~25-40 lines per copy
# ============================================================================

def gen_P04(vid, cid, rng, pvid):
    num_time_fields = 1 + (pvid % 3)
    has_bucket = (pvid + cid) % 2 == 0
    bucket_type = pvid % 3  # linear, log, custom
    c = fhdr(vid, cid)
    t0_off = rng.randint(0, 127)
    t1_off = rng.randint(128, 255)
    fi_base = rng.randint(0, 40)
    divisor = rng.uniform(60.0, 86400.0)
    c += f"  float viewerTime = {sr(t0_off)} * 1e6f;\n"
    c += f"  float storyTime = {sr(t1_off)} * 1e6f;\n"
    c += f"  float age = viewerTime - storyTime;\n"
    c += yd(fi_base, "age")
    inv_divisor = 1.0 / divisor
    c += f"  float ageHours = age * {inv_divisor:.10f}f;\n"
    c += yd(fi_base + 1, "ageHours")
    if has_bucket:
        c += f"  if (age > 0.0f) {{\n"
        if bucket_type == 0:  # linear
            bsize = rng.uniform(100.0, 10000.0)
            inv_bsize = 1.0 / bsize
            c += f"    int bucket = static_cast<int>(age * {inv_bsize:.10f}f);\n"
        elif bucket_type == 1:  # log — integer approximation
            c += f"    int bucket = static_cast<int>(31 - __builtin_clz(static_cast<uint32_t>(ageHours + 1.0f)));\n"
        else:  # custom thresholds
            thresholds = sorted([rng.uniform(0.1, 100.0) for _ in range(4)])
            c += f"    int bucket = 0;\n"
            for t_i, thr in enumerate(thresholds):
                c += f"    if (ageHours > {thr:.2f}f) bucket = {t_i + 1};\n"
        c += yi(fi_base + 2, "bucket", "1.0f")
        c += f"  }}\n"
    for tf in range(1, num_time_fields):
        off = rng.randint(0, 255)
        fi = fi_base + 3 + tf
        mul = rng.uniform(0.001, 1.0)
        c += f"  {{ float dt = {sr(off)} * {mul:.6f}f;\n"
        c += f"    if (dt > 0.0f) {{\n"
        c += yd(fi, "dt")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P05: Simple rate/counter yield (1 loop, 3-5 branches)
# ~35-50 lines per copy
# ============================================================================

def gen_P05(vid, cid, rng, pvid):
    num_counters = [3, 7, 11, 13][pvid % 4]
    num_rates = [5, 10, 15, 20][pvid % 4]
    has_imp_bucket = (pvid + cid) % 2 == 0
    has_hash_lookup = (pvid + cid) % 3 != 0
    c = fhdr(vid, cid)
    c += hash_block(rng, rng.randint(0, 255), num_ops=35)
    base_off = rng.randint(0, 100)
    fi_c = rng.randint(0, 30)
    fi_r = rng.randint(0, 30)
    null_off = rng.randint(0, 255)
    thr = rng.uniform(0.001, 0.1)
    c += f"  float null_chk = {sr(null_off)};\n"
    c += f"  if (null_chk < {thr:.4f}f) return;\n"
    # Hash lookup for additional data (adds 5-7 noinline hops)
    if has_hash_lookup:
        tbl = rng.randint(0, 3)
        c += f"  float hashBoost = {hl(tbl, sr(base_off))};\n"
    else:
        c += f"  float hashBoost = 1.0f;\n"
    # Counter loop — original inline code kept
    c += f"  for (int i = 0; i < {num_counters}; ++i) {{\n"
    c += f"    float cnt = {sr(base_off)} + c->structData[({base_off} + i) % c->structSize];\n"
    c += f"    cnt *= hashBoost;\n"
    c += f"    if (cnt > 0.0f) {{\n"
    c += yd(fi_c, "cnt")
    c += f"    }}\n"
    c += f"  }}\n"
    # Rate loop — original inline + computeRate helper
    imp_off = rng.randint(0, 255)
    c += f"  float denom = {sr(imp_off)};\n"
    c += f"  if (denom <= 0.0f) return;\n"
    c += f"  int32_t _den_b = *reinterpret_cast<int32_t*>(&denom);\n"
    c += f"  _den_b = 0x7EF311C2 - _den_b;\n"
    c += f"  float invDenom = *reinterpret_cast<float*>(&_den_b);\n"
    if has_imp_bucket:
        c += f"  int64_t impBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(denom + 1.0f)));\n"
    else:
        bucket_val = rng.randint(0, 15)
        c += f"  int64_t impBkt = {bucket_val}LL;\n"
    bucket_type = rng.randint(0, 3)
    c += f"  for (int i = 0; i < {num_rates}; ++i) {{\n"
    rate_off = rng.randint(0, 200)
    sc = rng.uniform(0.5, 2.0)
    c += f"    float rate = c->structData[({rate_off} + i) % c->structSize] * invDenom * {sc:.4f}f;\n"
    c += f"    rate = {cr('rate', '1.0f', bucket_type)};\n"  # Extra helper call
    c += yi(fi_r, "impBkt", "rate")
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P06: Dimensional rate yield (1-2 loops, 5-10 branches)
# ~50-75 lines per copy
# ============================================================================

def gen_P06(vid, cid, rng, pvid):
    num_dims = [2, 4, 7][pvid % 3]
    num_rates = [5, 10, 15][pvid % 3]
    c = fhdr(vid, cid)
    # Hash block with guards to increase BB size and conditional branch %
    c += hash_block(rng, rng.randint(0, 511), num_ops=35)
    dim_off = rng.randint(0, 255)
    c += f"  int dim = static_cast<int>({sr(dim_off)}) & 0x7;\n"
    c += f"  if (dim < 0 || dim >= {num_dims}) return;\n"
    # Impression per dimension
    imp_base = rng.randint(0, 200)
    c += f"  float dimImp = c->structData[({imp_base} + dim) % c->structSize];\n"
    c += f"  if (dimImp <= 0.0f) return;\n"
    c += f"  int32_t _di_b = *reinterpret_cast<int32_t*>(&dimImp);\n"
    c += f"  _di_b = 0x7EF311C2 - _di_b;\n"
    c += f"  float invImp = *reinterpret_cast<float*>(&_di_b);\n"
    c += f"  int64_t impBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(dimImp + 1.0f)));\n"
    # Rate loop
    rate_base = rng.randint(0, 150)
    fi_base = rng.randint(0, 30)
    c += f"  for (int i = 0; i < {num_rates}; ++i) {{\n"
    sc = rng.uniform(0.3, 2.5)
    c += f"    int off = ({rate_base} + dim * {num_rates} + i) % c->structSize;\n"
    c += f"    float rate = c->structData[off] * invImp * {sc:.4f}f;\n"
    c += f"    if (rate != 0.0f && std::isfinite(rate)) {{\n"
    c += f"      int fi = ({fi_base} + dim * {num_rates} + i) % c->numFeatures;\n"
    c += f"      if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
    c += f"        c->example->idScoreLists[fi].emplace_back(\n"
    c += f"          IdScorePair{{impBkt, rate}});\n"
    c += f"    }}\n"
    c += f"  }}\n"
    # Extra dimension-specific yields
    for d in range(min(num_dims, 3)):
        off = rng.randint(0, 255)
        fi = rng.randint(0, 49)
        thr = rng.uniform(0.0, 1.0)
        c += f"  if (dim == {d}) {{\n"
        c += f"    float extra = {sr(off)} * {rng.uniform(0.5, 2.0):.4f}f;\n"
        c += f"    if (extra > {thr:.4f}f) {{\n"
        c += yd(fi, "extra")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P07: Multi-conditional rate yield (2 loops, 8-15 branches)
# ~60-90 lines per copy
# ============================================================================

def gen_P07(vid, cid, rng, pvid):
    dims = [(2, 3), (3, 4), (4, 5), (5, 7)][pvid % 4]
    num_rates = [5, 10, 15][pvid % 3]
    has_calib = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    # Subject type classification
    s_off = rng.randint(0, 127)
    a_off = rng.randint(128, 255)
    c += f"  int subjType = static_cast<int>({sr(s_off)}) % {dims[0]};\n"
    c += f"  if (subjType < 0) subjType = 0;\n"
    c += f"  int actType = static_cast<int>({sr(a_off)}) % {dims[1]};\n"
    c += f"  if (actType < 0) actType = 0;\n"
    # 2D rate block access
    rate_base = rng.randint(0, 100)
    fi_base = rng.randint(0, 20)
    imp_off = rng.randint(0, 255)
    c += f"  float imp = {sr(imp_off)};\n"
    c += f"  if (imp <= 0.0f) return;\n"
    c += f"  int32_t _im_b = *reinterpret_cast<int32_t*>(&imp);\n"
    c += f"  _im_b = 0x7EF311C2 - _im_b;\n"
    c += f"  float invImp = *reinterpret_cast<float*>(&_im_b);\n"
    c += f"  int64_t impBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(imp + 1.0f)));\n"
    if has_calib:
        calib_off = rng.randint(0, 255)
        c += f"  float calibFactor = 1.0f + {sr(calib_off)} * 0.01f;\n"
    # Nested loop: subject type determines rate block, inner loop yields
    c += f"  for (int s = 0; s <= subjType; ++s) {{\n"
    c += f"    for (int i = 0; i < {num_rates}; ++i) {{\n"
    c += f"      int off = ({rate_base} + s * {dims[1]} * {num_rates}"
    c += f" + actType * {num_rates} + i) % c->structSize;\n"
    sc = rng.uniform(0.5, 2.0)
    c += f"      float rate = c->structData[off] * invImp * {sc:.4f}f;\n"
    c += f"      if (rate == 0.0f || !std::isfinite(rate)) continue;\n"
    if has_calib:
        c += f"      rate *= calibFactor;\n"
    c += f"      int fi = ({fi_base} + s * {num_rates} + i) % c->numFeatures;\n"
    c += f"      if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
    c += f"        c->example->idScoreLists[fi].emplace_back(\n"
    c += f"          IdScorePair{{impBkt, rate}});\n"
    c += f"    }}\n"
    c += f"  }}\n"
    # Extra branch for action type specific logic
    for a in range(min(dims[1], 3)):
        off = rng.randint(0, 255)
        fi = rng.randint(0, 49)
        c += f"  if (actType == {a}) {{\n"
        c += yd(fi, f"{sr(off)} * {rng.uniform(0.1, 3.0):.4f}f")
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P08: Single hash lookup + rates (1-2 loops, 5-10 branches)
# ~45-70 lines per copy
# ============================================================================

def gen_P08(vid, cid, rng, pvid):
    num_lookups = [1, 3, 5][pvid % 3]
    table_idx = pvid % 4
    c = fhdr(vid, cid)
    c += hash_block(rng, rng.randint(0, 127), num_ops=35)
    fi_base = rng.randint(0, 30)
    for lk in range(num_lookups):
        ki = rng.randint(0, 9)
        magic = rng.getrandbits(48)
        c += f"  {{ // lookup {lk}\n"
        c += f"    int64_t k = c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL;\n"
        # BOTH: original unordered_map lookup + mock hash table lookup
        c += f"    auto it = c->tables[{table_idx}].find(k);\n"
        c += f"    float baseVal = (it != c->tables[{table_idx}].end()) ? it->second : 0.0f;\n"
        c += f"    baseVal += {hl(table_idx, 'k')};\n"  # Add hash table call chain
        if lk == 0 and num_lookups == 1:
            c += f"    if (baseVal == 0.0f) return;\n"
        # Time slice loop — original inline + engagement stat helper
        ts_count = rng.randint(3, 7)
        c += f"    for (int ts = 0; ts < {ts_count}; ++ts) {{\n"
        thr = rng.uniform(0.01, 0.5)
        sc = rng.uniform(0.5, 3.0)
        stat_type = rng.randint(0, 7)
        c += f"      float sliceVal = baseVal * c->structData[({rng.randint(0, 200)}"
        c += f" + ts) % c->structSize] * {sc:.4f}f;\n"
        c += f"      sliceVal += helpers::computeEngagementStat("
        c += f"c->structData, c->structSize, ts, {stat_type}) * 0.01f;\n"
        c += f"      if (sliceVal > {thr:.4f}f) {{\n"
        fi_slot = rng.randint(0, 49)
        c += yd(fi_slot, "sliceVal")
        c += f"      }}\n"
        c += f"    }}\n"
        # Rate via helper
        rate_off = rng.randint(0, 255)
        fi_rate = rng.randint(0, 49)
        bucket_type = rng.randint(0, 3)
        c += f"    float weeks = {sr(rate_off)} * {rng.uniform(0.0001, 0.01):.6f}f;\n"
        c += f"    float rate = {cr('baseVal', 'weeks', bucket_type)};\n"
        c += yd(fi_rate, "rate")
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P09: Multi-key hash + dedup (2-3 nested loops, 10-20 branches)
# ~100-150 lines per copy
# ============================================================================

def gen_P09(vid, cid, rng, pvid):
    num_keys = [2, 3, 4][pvid % 3]
    num_ts = [3, 5, 7][pvid % 3]
    has_dedup = (pvid + cid) % 2 == 0
    has_agg = (pvid % 3) != 2
    has_ratio = (pvid % 5) == 0
    max_emit = 16
    max_agg = num_ts * max_emit
    c = fhdr(vid, cid)
    c += hash_block(rng, rng.randint(0, 255), num_ops=30)
    fi_base = rng.randint(0, 20)
    # Key derivation
    c += f"  int64_t keys[{num_keys}];\n"
    for k in range(num_keys):
        ki = rng.randint(0, 9)
        magic = rng.getrandbits(48)
        mix = rng.getrandbits(32)
        c += f"  keys[{k}] = c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL;\n"
        c += f"  keys[{k}] = (keys[{k}] >> 7) ^ (keys[{k}] * 0x{mix:08x}ULL);\n"
    if has_dedup:
        c += f"  bool emitted[{max_emit}] = {{}};\n"
    if has_agg:
        c += f"  float aggCounts[{max_agg}] = {{}};\n"
    # Outer key loop
    table_idx = pvid % 4
    c += f"  for (int k = 0; k < {num_keys}; ++k) {{\n"
    c += f"    auto it = c->tables[{table_idx}].find(keys[k]);\n"
    c += f"    if (it == c->tables[{table_idx}].end()) continue;\n"
    c += f"    float baseVal = it->second;\n"
    if has_dedup:
        c += f"    int emitIdx = static_cast<int>(static_cast<uint64_t>(keys[k]) % {max_emit});\n"
        c += f"    bool alreadyEmitted = emitted[emitIdx];\n"
        c += f"    emitted[emitIdx] = true;\n"
    # Inner time-slice loop
    c += f"    for (int ts = 0; ts < {num_ts}; ++ts) {{\n"
    decay_base = rng.uniform(0.7, 0.99)
    sc = rng.uniform(0.5, 2.0)
    c += f"      float decay = 1.0f;\n"
    c += f"      for (int d = 0; d < ts; ++d) decay *= {decay_base:.4f}f;\n"
    c += f"      float val = baseVal * decay * {sc:.4f}f;\n"
    thr = rng.uniform(0.001, 0.1)
    c += f"      if (val < {thr:.4f}f) continue;\n"
    if has_dedup:
        c += f"      if (!alreadyEmitted) {{\n"
        # Type dispatch
        type_a_count = rng.randint(1, num_keys - 1) if num_keys > 1 else 1
        c += f"        if (k < {type_a_count}) {{\n"
        fi_a = rng.randint(0, 49)
        c += yi(fi_a, f"ts", "val")
        c += f"        }} else {{\n"
        fi_b = rng.randint(0, 49)
        c += yi(fi_b, f"ts + {num_ts}", "val")
        c += f"        }}\n"
        if has_agg:
            c += f"      }} else {{\n"
            c += f"        int ai = (emitIdx * {num_ts} + ts) % {max_agg};\n"
            c += f"        aggCounts[ai] += val;\n"
        c += f"      }}\n"
    else:
        fi_x = rng.randint(0, 49)
        c += yi(fi_x, f"k * {num_ts} + ts", "val")
    c += f"    }}\n"
    # Per-key rate
    rate_off = rng.randint(0, 255)
    c += f"    float wks = {sr(rate_off)} * {rng.uniform(0.0001, 0.01):.6f}f;\n"
    c += f"    if (wks > 0.0f) {{\n"
    fi_rate = rng.randint(0, 49)
    c += f"      int32_t _wks_bits = *reinterpret_cast<int32_t*>(&wks);\n"
    c += f"      _wks_bits = 0x7EF311C2 - _wks_bits;\n"
    c += f"      float _inv_wks = *reinterpret_cast<float*>(&_wks_bits);\n"
    c += yd(fi_rate, f"baseVal * _inv_wks")
    c += f"    }}\n"
    c += f"  }}\n"  # end key loop
    # Yield aggregated
    if has_agg:
        fi_agg = rng.randint(0, 49)
        c += f"  for (int i = 0; i < {max_agg}; ++i) {{\n"
        c += f"    if (aggCounts[i] != 0.0f) {{\n"
        c += yi(fi_agg, "i", "aggCounts[i]")
        c += f"    }}\n"
        c += f"  }}\n"
    if has_ratio:
        fi_rat = rng.randint(0, 49)
        c += f"  {{ float sum0 = 0.0f, sum1 = 0.0f;\n"
        c += f"    for (int i = 0; i < {max_agg // 2}; ++i) sum0 += "
        c += f"c->structData[(i + {rng.randint(0, 200)}) % c->structSize];\n"
        c += f"    for (int i = 0; i < {max_agg // 2}; ++i) sum1 += "
        c += f"c->structData[(i + {rng.randint(0, 200)}) % c->structSize];\n"
        c += f"    if (sum1 > 0.0f) {{\n"
        c += f"      int32_t _s1_bits = *reinterpret_cast<int32_t*>(&sum1);\n"
        c += f"      _s1_bits = 0x7EF311C2 - _s1_bits;\n"
        c += f"      float _inv_s1 = *reinterpret_cast<float*>(&_s1_bits);\n"
        c += yd(fi_rat, "sum0 * _inv_s1")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P10: Dual hash lookup (2 loops, 8-15 branches)
# ~60-80 lines per copy
# ============================================================================

def gen_P10(vid, cid, rng, pvid):
    num_rates = [3, 5, 8][pvid % 3]
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 20)
    # First hash lookup (state table)
    ki0 = rng.randint(0, 9)
    magic0 = rng.getrandbits(48)
    t0 = pvid % 4
    c += f"  int64_t storyKey = c->queryKeys[{ki0} % c->numKeys] ^ 0x{magic0:012x}LL;\n"
    c += f"  auto stateIt = c->tables[{t0}].find(storyKey);\n"
    c += f"  bool wasRead = (stateIt != c->tables[{t0}].end() && stateIt->second > 0.5f);\n"
    # Second hash lookup (FIH table)
    ki1 = rng.randint(0, 9)
    magic1 = rng.getrandbits(48)
    t1 = (pvid + 1) % 4
    c += f"  int64_t fihKey = c->queryKeys[{ki1} % c->numKeys] ^ 0x{magic1:012x}LL;\n"
    c += f"  auto fihIt = c->tables[{t1}].find(fihKey);\n"
    c += f"  if (fihIt == c->tables[{t1}].end()) return;\n"
    c += f"  float fihBase = fihIt->second;\n"
    # Conditional rate paths
    c += f"  if (wasRead) {{\n"
    for i in range(num_rates):
        off = rng.randint(0, 255)
        fi = fi_base + i
        sc = rng.uniform(0.3, 2.0)
        c += f"    {{ float rate = fihBase * {sr(off)} * {sc:.4f}f;\n"
        c += yd(fi, "rate")
        c += f"    }}\n"
    c += f"  }} else {{\n"
    for i in range(num_rates):
        off = rng.randint(0, 255)
        fi = fi_base + num_rates + i
        sc = rng.uniform(0.3, 2.0)
        c += f"    {{ float rate = fihBase * {sr(off)} * {sc:.4f}f;\n"
        c += yd(fi, "rate")
        c += f"    }}\n"
    c += f"  }}\n"
    # Extra conditional yields
    thr = rng.uniform(0.1, 0.9)
    fi_ex = rng.randint(0, 49)
    c += f"  if (fihBase > {thr:.4f}f) {{\n"
    c += yd(fi_ex, f"fihBase * {rng.uniform(1.0, 5.0):.4f}f")
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P11: Hash + tree traversal (2 loops, 8-15 branches)
# Simulates red-black tree walk using struct array as linked list
# ~80-120 lines per copy
# ============================================================================

def gen_P11(vid, cid, rng, pvid):
    map_size = [50, 100, 200][pvid % 3]
    num_chains = 1 + (pvid % 3)
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 30)
    # Hash lookup for root
    ki = rng.randint(0, 9)
    magic = rng.getrandbits(48)
    tidx = pvid % 4
    c += f"  int64_t rootKey = c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL;\n"
    c += f"  auto rootIt = c->tables[{tidx}].find(rootKey);\n"
    c += f"  if (rootIt == c->tables[{tidx}].end()) return;\n"
    c += f"  float rootVal = rootIt->second;\n"
    # Simulated tree traversal: chase indices through structData
    c += f"  // Simulated tree traversal (pointer chasing)\n"
    c += f"  int nodeIdx = static_cast<int>(rootVal * {map_size}.0f) % c->structSize;\n"
    c += f"  float treeSum = 0.0f;\n"
    c += f"  int treeCount = 0;\n"
    max_depth = rng.randint(15, 40)
    thr = rng.uniform(0.01, 0.5)
    c += f"  for (int depth = 0; depth < {max_depth}; ++depth) {{\n"
    c += f"    float nodeVal = c->structData[nodeIdx % c->structSize];\n"
    c += f"    if (nodeVal < {thr:.4f}f) break;  // null node\n"
    # Left/right decision based on node value
    c += f"    if (nodeVal > 0.5f) {{\n"
    c += f"      nodeIdx = (nodeIdx * 2 + 1) % c->structSize;  // left child\n"
    c += f"    }} else {{\n"
    c += f"      nodeIdx = (nodeIdx * 2 + 2) % c->structSize;  // right child\n"
    c += f"    }}\n"
    c += f"    treeSum += nodeVal;\n"
    c += f"    treeCount++;\n"
    # Yield per node
    fi_node = rng.randint(0, 49)
    c += yi(fi_node, "static_cast<int64_t>(nodeIdx)", "nodeVal")
    c += f"  }}\n"
    # Summary yields
    c += f"  if (treeCount > 0) {{\n"
    c += yd(fi_base, "treeSum")
    c += f"    float _tcf = static_cast<float>(treeCount);\n"
    c += f"    int32_t _tc_bits = *reinterpret_cast<int32_t*>(&_tcf);\n"
    c += f"    _tc_bits = 0x7EF311C2 - _tc_bits;\n"
    c += f"    float _inv_tc = *reinterpret_cast<float*>(&_tc_bits);\n"
    c += yd(fi_base + 1, f"treeSum * _inv_tc")
    c += f"  }}\n"
    # Additional chains
    for ch in range(1, num_chains):
        ki2 = rng.randint(0, 9)
        magic2 = rng.getrandbits(48)
        tidx2 = (pvid + ch) % 4
        c += f"  {{ // chain {ch}\n"
        c += f"    int64_t k = c->queryKeys[{ki2} % c->numKeys] ^ 0x{magic2:012x}LL;\n"
        c += f"    auto it = c->tables[{tidx2}].find(k);\n"
        c += f"    if (it != c->tables[{tidx2}].end()) {{\n"
        c += f"      float v = it->second;\n"
        chain_depth = rng.randint(5, 15)
        c += f"      int ci = static_cast<int>(v * {rng.randint(50, 200)}.0f) % c->structSize;\n"
        c += f"      for (int d = 0; d < {chain_depth}; ++d) {{\n"
        c += f"        float nv = c->structData[ci % c->structSize];\n"
        c += f"        if (nv < 0.01f) break;\n"
        c += f"        ci = (ci + static_cast<int>(nv * 100.0f)) % c->structSize;\n"
        fi_ch = rng.randint(0, 49)
        c += yi(fi_ch, f"static_cast<int64_t>(ci)", "nv")
        c += f"      }}\n"
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P12: Template time-window stats (1 loop, 2-3 branches)
# ~30-40 lines per copy
# ============================================================================

def gen_P12(vid, cid, rng, pvid):
    num_stats = [5, 10, 15, 20, 25, 30][pvid % 6]
    window_off = rng.randint(0, 200)
    c = fhdr(vid, cid)
    # Hash block (P12 is most frequent: 130 variants)
    c += hash_block(rng, rng.randint(0, 255), num_ops=35)
    fi_base = rng.randint(0, 30)
    null_off = rng.randint(0, 255)
    thr = rng.uniform(0.0, 0.05)
    c += f"  constexpr int kVariantId = {vid};\n"
    c += f"  float null_chk = {sr(null_off)} + _hf;\n"
    c += f"  if (null_chk < {thr:.4f}f) return;\n"
    # Hash lookup for base multiplier (5-7 noinline hops)
    use_hash = (pvid + cid) % 3 != 0
    if use_hash:
        tbl = rng.randint(0, 3)
        c += f"  float statBase = {ls(tbl, pvid % 8)};\n"
    else:
        c += f"  float statBase = 1.0f;\n"
    sc = rng.uniform(0.5, 3.0)
    # Original inline stat loop + helper validation
    c += f"  for (int i = 0; i < {num_stats}; ++i) {{\n"
    c += f"    float val = c->structData[({window_off} + kVariantId * {num_stats}"
    c += f" + i) % c->structSize] * {sc:.4f}f;\n"
    c += f"    val *= statBase;\n"
    c += f"    val += helpers::computeEngagementStat("
    c += f"c->structData, c->structSize, i, ({window_off} + kVariantId) % 8) * 0.01f;\n"
    c += f"    if (val != 0.0f) {{\n"
    c += yd(fi_base, "val")
    c += f"    }}\n"
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P13: Massive coefficient yield (1-2 loops, 10-15 branches)
# 100-250 explicit feature yields per copy
# ~150-300 lines per copy
# ============================================================================

def gen_P13(vid, cid, rng, pvid):
    num_imp_feats = 80 + pvid * 30  # 80-200
    has_vpv = pvid % 2 == 0
    num_vpv_feats = 30 + pvid * 15 if has_vpv else 0
    c = fhdr(vid, cid)
    imp_off = rng.randint(0, 255)
    c += f"  float imp = {sr(imp_off)};\n"
    c += f"  if (imp <= 0.0f) return;\n"
    eps = rng.uniform(0.001, 0.1)
    c += f"  float _imp_d = imp + {eps:.6f}f;\n"
    c += f"  int32_t _imp_b = *reinterpret_cast<int32_t*>(&_imp_d);\n"
    c += f"  _imp_b = 0x7EF311C2 - _imp_b;\n"
    c += f"  float invImp = *reinterpret_cast<float*>(&_imp_b);\n"
    bucket_magic = rng.getrandbits(16)
    c += f"  int64_t impBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(imp + 1.0f)))"
    c += f" ^ 0x{bucket_magic:04x}LL;\n"
    # Block 1: impression-indexed rates (unrolled)
    c += f"  // Block 1: {num_imp_feats} impression-indexed yields\n"
    for i in range(num_imp_feats):
        off = rng.randint(0, 1023)
        fi = rng.randint(0, 49)
        sc = rng.uniform(0.1, 5.0)
        # Vary the computation between yields — integer ops only
        comp = rng.randint(0, 3)
        if comp == 0:
            val_expr = f"c->structData[{off} % c->structSize] * invImp * {sc:.4f}f"
        elif comp == 1:
            off2 = rng.randint(0, 1023)
            val_expr = (f"(c->structData[{off} % c->structSize] - "
                       f"c->structData[{off2} % c->structSize]) * invImp * {sc:.4f}f")
        elif comp == 2:
            val_expr = int_hash_expr(rng, f"c->structData[{off} % c->structSize] * invImp")
        else:
            val_expr = f"c->structData[{off} % c->structSize] * invImp * invImp * {sc:.4f}f"
        c += f"  {{ float r = {val_expr};\n"
        c += f"    if (r != 0.0f && std::isfinite(r)) {{\n"
        c += f"      int _fi = {fi} % c->numFeatures;\n"
        c += f"      if (_fi >= 0 && _fi < (int)c->example->idScoreLists.size())\n"
        c += f"        c->example->idScoreLists[_fi].emplace_back(\n"
        c += f"          IdScorePair{{impBkt, r}});\n"
        c += f"    }}\n"
        c += f"  }}\n"
    # Block 2: VPV rates
    if has_vpv:
        vpv_off = rng.randint(0, 255)
        c += f"  float vpv = {sr(vpv_off)};\n"
        c += f"  if (vpv > 0.0f) {{\n"
        c += f"    int32_t _vpv_b = *reinterpret_cast<int32_t*>(&vpv);\n"
        c += f"    _vpv_b = 0x7EF311C2 - _vpv_b;\n"
        c += f"    float invVpv = *reinterpret_cast<float*>(&_vpv_b);\n"
        c += f"    int64_t vpvBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(vpv + 1.0f)));\n"
        for i in range(num_vpv_feats):
            off = rng.randint(0, 1023)
            fi = rng.randint(0, 49)
            sc = rng.uniform(0.1, 5.0)
            c += f"    {{ float r = c->structData[{off} % c->structSize] * invVpv * {sc:.4f}f;\n"
            c += f"      if (r != 0.0f && std::isfinite(r)) {{\n"
            c += f"        int _fi = {fi} % c->numFeatures;\n"
            c += f"        if (_fi >= 0 && _fi < (int)c->example->idScoreLists.size())\n"
            c += f"          c->example->idScoreLists[_fi].emplace_back(\n"
            c += f"            IdScorePair{{vpvBkt, r}});\n"
            c += f"      }}\n"
            c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P14: Decayed coefficient (1-2 loops, 6-12 branches)
# Like P13 but with exponential decay
# ~80-150 lines per copy
# ============================================================================

def gen_P14(vid, cid, rng, pvid):
    num_feats = [30, 50, 80, 100][pvid % 4]
    decay_type = pvid % 3  # exponential, linear, half-life
    c = fhdr(vid, cid)
    imp_off = rng.randint(0, 255)
    time_off = rng.randint(0, 255)
    c += f"  float imp = {sr(imp_off)};\n"
    c += f"  if (imp <= 0.0f) return;\n"
    c += f"  int32_t _i14_b = *reinterpret_cast<int32_t*>(&imp);\n"
    c += f"  _i14_b = 0x7EF311C2 - _i14_b;\n"
    c += f"  float invImp = *reinterpret_cast<float*>(&_i14_b);\n"
    time_sc = rng.uniform(0.01, 10.0)
    c += f"  float _ts_raw = {sr(time_off)};\n"
    c += f"  float timeSince = (_ts_raw > 0.0f ? _ts_raw : -_ts_raw) * {time_sc:.4f}f;\n"
    # Decay factor — integer approximation replacing std::exp
    if decay_type == 0:
        decay_rate = rng.uniform(0.001, 0.1)
        c += f"  float _dec_den = 1.0f + {decay_rate:.6f}f * timeSince;\n"
        c += f"  int32_t _dec_b = *reinterpret_cast<int32_t*>(&_dec_den);\n"
        c += f"  _dec_b = 0x7EF311C2 - _dec_b;\n"
        c += f"  float decay = *reinterpret_cast<float*>(&_dec_b);\n"
    elif decay_type == 1:
        ts_sc = rng.uniform(0.01, 0.5)
        c += f"  float _dec_den = 1.0f + timeSince * {ts_sc:.4f}f;\n"
        c += f"  int32_t _dec_b = *reinterpret_cast<int32_t*>(&_dec_den);\n"
        c += f"  _dec_b = 0x7EF311C2 - _dec_b;\n"
        c += f"  float decay = *reinterpret_cast<float*>(&_dec_b);\n"
    else:
        half_life = rng.uniform(1.0, 168.0)
        c += f"  float _dec_den = 1.0f + 0.693147f * timeSince * {1.0/half_life:.6f}f;\n"
        c += f"  int32_t _dec_b = *reinterpret_cast<int32_t*>(&_dec_den);\n"
        c += f"  _dec_b = 0x7EF311C2 - _dec_b;\n"
        c += f"  float decay = *reinterpret_cast<float*>(&_dec_b);\n"
    c += f"  int64_t impBkt = static_cast<int64_t>(31 - __builtin_clz(static_cast<uint32_t>(imp + 1.0f)));\n"
    # Feature yields with decay
    for i in range(num_feats):
        off = rng.randint(0, 511)
        fi = rng.randint(0, 49)
        sc = rng.uniform(0.1, 5.0)
        c += f"  {{ float r = c->structData[{off} % c->structSize] * invImp * decay * {sc:.4f}f;\n"
        c += f"    if (r != 0.0f && std::isfinite(r)) {{\n"
        c += f"      int _fi = {fi} % c->numFeatures;\n"
        c += f"      if (_fi >= 0 && _fi < (int)c->example->idScoreLists.size())\n"
        c += f"        c->example->idScoreLists[_fi].emplace_back(\n"
        c += f"          IdScorePair{{impBkt, r}});\n"
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P15: Embedding dimension yield (1-2 loops, 3-6 branches)
# ~45-70 lines per copy
# ============================================================================

def gen_P15(vid, cid, rng, pvid):
    num_emb_types = [1, 2, 4, 6][pvid % 4]
    emb_dim = [32, 64, 128][pvid % 3]
    has_resolution = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 20)
    # Outer loop: embedding types
    for et in range(num_emb_types):
        tidx = et % 4
        ki = rng.randint(0, 9)
        magic = rng.getrandbits(48)
        c += f"  {{ // embedding type {et}\n"
        if has_resolution:
            res_off = rng.randint(0, 255)
            c += f"    int64_t entityId = static_cast<int64_t>({sr(res_off)} * 1000.0f)"
            c += f" ^ 0x{rng.getrandbits(32):08x}LL;\n"
        else:
            c += f"    int64_t entityId = c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL;\n"
        c += f"    auto it = c->tables[{tidx}].find(entityId);\n"
        c += f"    if (it != c->tables[{tidx}].end()) {{\n"
        c += f"      float base = it->second;\n"
        # Dimension loop
        c += f"      for (int d = 0; d < {emb_dim}; ++d) {{\n"
        off = rng.randint(0, 200)
        sc = rng.uniform(0.1, 2.0)
        c += f"        float dimVal = base * c->structData[({off} + d) % c->structSize] * {sc:.4f}f;\n"
        c += f"        int fi = ({fi_base} + {et} * {emb_dim} + d) % c->numFeatures;\n"
        c += f"        if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
        c += f"          c->example->idScoreLists[fi].emplace_back(\n"
        c += f"            IdScorePair{{static_cast<int64_t>(d), dimVal}});\n"
        c += f"      }}\n"
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P16: Embedding similarity (1-2 loops, 4-8 branches)
# ~50-70 lines per copy
# ============================================================================

def gen_P16(vid, cid, rng, pvid):
    emb_dim = [32, 64, 128][pvid % 3]
    sim_type = pvid % 3  # dot, cosine, l2
    yield_dims = (pvid + cid) % 3 == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 30)
    vw_off = rng.randint(0, 100)
    ct_off = rng.randint(128, 250)
    c += f"  float dotProduct = 0.0f;\n"
    if sim_type == 1:  # cosine
        c += f"  float normA = 0.0f, normB = 0.0f;\n"
    elif sim_type == 2:  # L2
        c += f"  float l2sum = 0.0f;\n"
    c += f"  for (int i = 0; i < {emb_dim}; ++i) {{\n"
    c += f"    float a = c->structData[({vw_off} + i) % c->structSize];\n"
    c += f"    float b = c->structData[({ct_off} + i) % c->structSize];\n"
    c += f"    dotProduct += a * b;\n"
    if sim_type == 1:
        c += f"    normA += a * a;\n"
        c += f"    normB += b * b;\n"
    elif sim_type == 2:
        c += f"    float diff = a - b;\n"
        c += f"    l2sum += diff * diff;\n"
    c += f"  }}\n"
    # Compute similarity — integer approximations replacing std::sqrt
    if sim_type == 0:
        c += f"  float similarity = dotProduct;\n"
    elif sim_type == 1:
        # Use integer bit manipulation for fast inverse sqrt approximation
        c += f"  float normProd = normA * normB + 1e-8f;\n"
        c += f"  int32_t _npi = *reinterpret_cast<int32_t*>(&normProd);\n"
        c += f"  _npi = 0x5F3759DF - (_npi >> 1);\n"
        c += f"  float invSqrtNorm = *reinterpret_cast<float*>(&_npi);\n"
        c += f"  float similarity = dotProduct * invSqrtNorm;\n"
    else:
        # fast sqrt via inverse sqrt: sqrt(x) = x * rsqrt(x)
        c += f"  float _l2t = l2sum + 1e-8f;\n"
        c += f"  int32_t _l2i = *reinterpret_cast<int32_t*>(&_l2t);\n"
        c += f"  _l2i = 0x5F3759DF - (_l2i >> 1);\n"
        c += f"  float _l2r = *reinterpret_cast<float*>(&_l2i);\n"
        c += f"  float similarity = _l2t * _l2r;\n"
    c += yd(fi_base, "similarity")
    # Optional dimension yield
    if yield_dims:
        c += f"  for (int i = 0; i < {emb_dim}; ++i) {{\n"
        c += f"    float dimVal = c->structData[({ct_off} + i) % c->structSize];\n"
        fi_dim = rng.randint(0, 49)
        c += f"    int fi = ({fi_dim} + i) % c->numFeatures;\n"
        c += f"    if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
        c += f"      c->example->idScoreLists[fi].emplace_back(\n"
        c += f"        IdScorePair{{static_cast<int64_t>(i), dimVal}});\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P17: Sparse feature forward (1 loop, 2-3 branches)
# ~30-40 lines per copy
# ============================================================================

def gen_P17(vid, cid, rng, pvid):
    num_lists = [1, 3, 5, 10][pvid % 4]
    items_per_list = [10, 30, 50][pvid % 3]
    has_filter = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 30)
    threshold = rng.uniform(0.01, 0.5)
    for lst in range(num_lists):
        base_off = rng.randint(0, 200)
        c += f"  {{ // sparse list {lst}\n"
        c += f"    for (int j = 0; j < {items_per_list}; ++j) {{\n"
        sc = rng.uniform(0.5, 3.0)
        c += f"      int off = ({base_off} + j * 2) % c->structSize;\n"
        c += f"      int64_t id = static_cast<int64_t>(c->structData[off] * 1000.0f);\n"
        c += f"      float score = c->structData[(off + 1) % c->structSize] * {sc:.4f}f;\n"
        if has_filter:
            c += f"      if (score < {threshold:.4f}f) continue;\n"
        c += f"      int fi = ({fi_base} + {lst}) % c->numFeatures;\n"
        c += f"      if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
        c += f"        c->example->idScoreLists[fi].emplace_back(\n"
        c += f"          IdScorePair{{id, score}});\n"
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P18: Hash-then-sparse forward (2 loops, 5-8 branches)
# ~40-55 lines per copy
# ============================================================================

def gen_P18(vid, cid, rng, pvid):
    num_items = [10, 20, 30, 50][pvid % 4]
    c = fhdr(vid, cid)
    fi = rng.randint(0, 49)
    ki = rng.randint(0, 9)
    magic = rng.getrandbits(48)
    tidx = pvid % 4
    c += f"  int64_t key = c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL;\n"
    c += f"  auto it = c->tables[{tidx}].find(key);\n"
    c += f"  if (it == c->tables[{tidx}].end()) return;\n"
    c += f"  float base = it->second;\n"
    base_off = rng.randint(0, 200)
    sc = rng.uniform(0.5, 3.0)
    thr = rng.uniform(0.01, 0.3)
    c += f"  for (int j = 0; j < {num_items}; ++j) {{\n"
    c += f"    int off = ({base_off} + j * 2) % c->structSize;\n"
    c += f"    int64_t id = static_cast<int64_t>(c->structData[off] * 1e4f);\n"
    c += f"    float score = base * c->structData[(off + 1) % c->structSize] * {sc:.4f}f;\n"
    c += f"    if (score < {thr:.4f}f) continue;\n"
    c += f"    if ({fi} >= 0 && {fi} < (int)c->example->idScoreLists.size())\n"
    c += f"      c->example->idScoreLists[{fi}].emplace_back(\n"
    c += f"        IdScorePair{{id, score}});\n"
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P19: LastN ID match (1 loop, 1-3 branches)
# ~30-40 lines per copy
# ============================================================================

def gen_P19(vid, cid, rng, pvid):
    list_size = [10, 20, 50, 100][pvid % 4]
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 40)
    target_ki = rng.randint(0, 9)
    c += f"  int64_t targetId = c->queryKeys[{target_ki} % c->numKeys];\n"
    c += f"  int matchPos = -1;\n"
    list_off = rng.randint(0, 200)
    c += f"  for (int i = 0; i < {list_size}; ++i) {{\n"
    c += f"    int64_t recentId = static_cast<int64_t>(\n"
    c += f"      c->structData[({list_off} + i) % c->structSize] * 1e6f);\n"
    c += f"    if (recentId == targetId) {{ matchPos = i; break; }}\n"
    c += f"  }}\n"
    c += f"  if (matchPos >= 0) {{\n"
    c += yd(fi_base, "1.0f")
    c += yd(fi_base + 1, f"static_cast<float>(matchPos)")
    recency_sc = rng.uniform(0.5, 2.0)
    c += f"    float _mp_den = 1.0f + static_cast<float>(matchPos);\n"
    c += f"    int32_t _mp_bits = *reinterpret_cast<int32_t*>(&_mp_den);\n"
    c += f"    _mp_bits = 0x7EF311C2 - _mp_bits;\n"
    c += f"    float _inv_mp = *reinterpret_cast<float*>(&_mp_bits);\n"
    c += yd(fi_base + 2, f"{recency_sc:.4f}f * _inv_mp")
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P20: Set membership match (1 loop, 3-6 branches)
# ~30-40 lines per copy
# ============================================================================

def gen_P20(vid, cid, rng, pvid):
    num_sets = [1, 3, 5][pvid % 3]
    set_size = [10, 30, 50][pvid % 3]
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 40)
    target_ki = rng.randint(0, 9)
    c += f"  int64_t actorId = c->queryKeys[{target_ki} % c->numKeys];\n"
    for s in range(num_sets):
        set_off = rng.randint(0, 200)
        fi = fi_base + s
        c += f"  {{ // set {s}\n"
        c += f"    bool isMember = false;\n"
        c += f"    for (int i = 0; i < {set_size}; ++i) {{\n"
        c += f"      int64_t memberId = static_cast<int64_t>(\n"
        c += f"        c->structData[({set_off} + i) % c->structSize] * 1e6f);\n"
        c += f"      if (memberId == actorId) {{ isMember = true; break; }}\n"
        c += f"    }}\n"
        c += f"    if (isMember) {{\n"
        c += yd(fi, "1.0f")
        c += f"    }}\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P21: Large monolithic prediction (3-5 loops, 30-50 branches)
# ~180-250 lines per copy
# ============================================================================

def gen_P21(vid, cid, rng, pvid):
    num_switch_cases = [10, 20, 30][pvid % 3]
    num_calib_tables = [3, 5, 8][pvid % 3]
    num_predictions = [50, 100, 150][pvid % 3]
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 10)

    # Event type classification (large switch)
    type_off = rng.randint(0, 255)
    c += f"  int eventType = static_cast<int>({sr(type_off)} * {num_switch_cases}.0f)"
    c += f" % {num_switch_cases};\n"
    c += f"  float eventWeight = 1.0f;\n"
    c += f"  switch (eventType) {{\n"
    for case_i in range(num_switch_cases):
        weight = rng.uniform(0.1, 5.0)
        c += f"    case {case_i}: eventWeight = {weight:.4f}f; break;\n"
    c += f"    default: eventWeight = 1.0f; break;\n"
    c += f"  }}\n"

    # Calibration tables
    for ct in range(num_calib_tables):
        c += f"  // Calibration table {ct}\n"
        c += f"  float calib{ct} = 1.0f;\n"
        calib_off = rng.randint(0, 255)
        num_calib_entries = rng.randint(3, 8)
        c += f"  {{ float raw = {sr(calib_off)};\n"
        for ce in range(num_calib_entries):
            thr = rng.uniform(-2.0, 2.0)
            factor = rng.uniform(0.5, 2.0)
            c += f"    if (raw > {thr:.4f}f) calib{ct} = {factor:.4f}f;\n"
        c += f"  }}\n"

    # Hash lookups for base predictions
    for lk in range(min(3, num_calib_tables)):
        ki = rng.randint(0, 9)
        magic = rng.getrandbits(48)
        tidx = lk % 4
        c += f"  float pred{lk} = 0.0f;\n"
        c += f"  {{ auto it = c->tables[{tidx}].find(\n"
        c += f"      c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL);\n"
        c += f"    if (it != c->tables[{tidx}].end()) pred{lk} = it->second;\n"
        c += f"  }}\n"

    # Prediction loop with calibration
    c += f"  for (int p = 0; p < {num_predictions}; ++p) {{\n"
    pred_off = rng.randint(0, 200)
    c += f"    float rawPred = c->structData[({pred_off} + p) % c->structSize];\n"
    c += f"    if (rawPred == 0.0f) continue;\n"
    c += f"    float calibrated = rawPred * eventWeight;\n"
    for ct in range(num_calib_tables):
        c += f"    calibrated *= calib{ct};\n"
    # Conditional calibration adjustments
    for adj in range(3):
        thr = rng.uniform(-1.0, 1.0)
        factor = rng.uniform(0.8, 1.2)
        c += f"    if (calibrated > {thr:.4f}f) calibrated *= {factor:.4f}f;\n"
    # Transform — integer-only replacements
    transform = rng.choice([
        "(calibrated > 3.0f ? 1.0f : (calibrated < -3.0f ? -1.0f : calibrated * 0.33f))",
        "(calibrated > 0.0f ? calibrated : -calibrated)",
        "calibrated",
        "static_cast<float>(static_cast<int32_t>(calibrated * 1000.0f) & 0x7FFFFFFF) * 0.001f",
    ])
    c += f"    float final_p = {transform};\n"
    c += f"    int fi = ({fi_base} + p) % c->numFeatures;\n"
    c += f"    int32_t idx = c->features[fi].raw_feature_index;\n"
    c += f"    if (idx >= 0 && idx < (int32_t)c->example->denseValues.size())\n"
    c += f"      c->example->denseValues[idx] = final_p;\n"
    c += f"  }}\n"

    # Feature mapping loop
    c += f"  for (int m = 0; m < {num_predictions // 2}; ++m) {{\n"
    map_off = rng.randint(0, 200)
    c += f"    float mapped = c->structData[({map_off} + m) % c->structSize];\n"
    c += f"    if (mapped == 0.0f) continue;\n"
    # Nested branch for mapping type
    c += f"    int mapType = m % {min(5, num_calib_tables)};\n"
    for mt in range(min(5, num_calib_tables)):
        sc = rng.uniform(0.5, 3.0)
        c += f"    {'if' if mt == 0 else 'else if'} (mapType == {mt}) {{\n"
        c += f"      mapped *= {sc:.4f}f * calib{mt % num_calib_tables};\n"
        fi = rng.randint(0, 49)
        c += yd(fi, "mapped")
        c += f"    }}\n"
    c += f"  }}\n"

    # Score combination
    c += f"  float combined = pred0"
    for lk in range(1, min(3, num_calib_tables)):
        w = rng.uniform(0.1, 2.0)
        c += f" + pred{lk} * {w:.4f}f"
    c += f";\n"
    c += f"  combined *= eventWeight;\n"
    fi_final = rng.randint(0, 49)
    c += yd(fi_final, "combined")

    c += "}\n\n"
    return c


# ============================================================================
# Pattern P22: Medium prediction forward (1-2 loops, 5-15 branches)
# ~60-100 lines per copy
# ============================================================================

def gen_P22(vid, cid, rng, pvid):
    num_preds = [10, 30, 50, 100][pvid % 4]
    lookup_type = pvid % 3  # hash, array, switch
    has_calib = (pvid + cid) % 2 == 0
    has_fallback = (pvid % 3) == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 20)
    # Prediction source
    if lookup_type == 0:  # hash
        ki = rng.randint(0, 9)
        magic = rng.getrandbits(48)
        tidx = pvid % 4
        c += f"  auto baseIt = c->tables[{tidx}].find(\n"
        c += f"    c->queryKeys[{ki} % c->numKeys] ^ 0x{magic:012x}LL);\n"
        c += f"  float basePred = (baseIt != c->tables[{tidx}].end()) ? baseIt->second : 0.0f;\n"
    else:
        off = rng.randint(0, 255)
        c += f"  float basePred = {sr(off)};\n"
    if has_calib:
        c += f"  float calibScale = 1.0f + {sr(rng.randint(0, 255))} * 0.05f;\n"
        c += f"  float calibBias = {sr(rng.randint(0, 255))} * 0.01f;\n"
    if has_fallback:
        fallback = rng.uniform(0.01, 0.5)
        c += f"  float fallback = {fallback:.4f}f;\n"
    # Prediction loop
    c += f"  for (int p = 0; p < {num_preds}; ++p) {{\n"
    pred_off = rng.randint(0, 200)
    c += f"    float pred = c->structData[({pred_off} + p) % c->structSize];\n"
    if has_fallback:
        c += f"    if (pred == 0.0f) pred = fallback;\n"
    c += f"    pred *= basePred;\n"
    if has_calib:
        c += f"    pred = pred * calibScale + calibBias;\n"
    c += f"    if (pred != 0.0f && std::isfinite(pred)) {{\n"
    c += f"      int fi = ({fi_base} + p) % c->numFeatures;\n"
    c += f"      int32_t idx = c->features[fi].raw_feature_index;\n"
    c += f"      if (idx >= 0 && idx < (int32_t)c->example->denseValues.size())\n"
    c += f"        c->example->denseValues[idx] = pred;\n"
    c += f"    }}\n"
    c += f"  }}\n"
    if lookup_type == 2:  # switch-based extra yields
        num_cases = rng.randint(3, 7)
        type_off = rng.randint(0, 255)
        c += f"  int predType = static_cast<int>({sr(type_off)}) % {num_cases};\n"
        c += f"  switch (predType) {{\n"
        for cs in range(num_cases):
            fi = rng.randint(0, 49)
            sc = rng.uniform(0.5, 3.0)
            c += f"    case {cs}:\n"
            c += yd(fi, f"basePred * {sc:.4f}f")
            c += f"      break;\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P23: SentencePiece tokenization (2-3 loops, 5-10 branches)
# ~70-110 lines per copy
# ============================================================================

def gen_P23(vid, cid, rng, pvid):
    max_tokens = [50, 100, 200][pvid % 3]
    has_bigram = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 30)
    # Hash-based tokenization from struct data (simulating text)
    text_off = rng.randint(0, 200)
    vocab = rng.randint(5000, 30000)
    c += f"  // Hash-based tokenization\n"
    c += f"  int tokens[{max_tokens}];\n"
    c += f"  int numTokens = 0;\n"
    c += f"  uint64_t hashState = {rng.getrandbits(64)}ULL;\n"
    c += f"  for (int i = 0; i < {max_tokens}; ++i) {{\n"
    c += f"    float charVal = c->structData[({text_off} + i) % c->structSize];\n"
    c += f"    if (charVal < 0.01f) break;\n"
    c += f"    hashState = hashState * 6364136223846793005ULL + "
    c += f"static_cast<uint64_t>(charVal * 1e6f);\n"
    c += f"    tokens[numTokens++] = static_cast<int>(hashState % {vocab}ULL);\n"
    c += f"  }}\n"
    # Yield unigram tokens
    c += f"  for (int i = 0; i < numTokens; ++i) {{\n"
    c += f"    int fi = ({fi_base} + 0) % c->numFeatures;\n"
    c += f"    if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
    c += f"      c->example->idScoreLists[fi].emplace_back(\n"
    c += f"        IdScorePair{{static_cast<int64_t>(tokens[i]), 1.0f}});\n"
    c += f"  }}\n"
    # Optional bigram loop
    if has_bigram:
        c += f"  for (int i = 0; i + 1 < numTokens; ++i) {{\n"
        c += f"    int64_t bigram = (static_cast<int64_t>(tokens[i]) << 16)"
        c += f" | tokens[i + 1];\n"
        fi_bi = rng.randint(0, 49)
        c += f"    int fi = ({fi_bi} + 1) % c->numFeatures;\n"
        c += f"    if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
        c += f"      c->example->idScoreLists[fi].emplace_back(\n"
        c += f"        IdScorePair{{bigram, 1.0f}});\n"
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P24: Mutable state read/update (0-2 loops, 5-10 branches)
# ~40-60 lines per copy
# ============================================================================

def gen_P24(vid, cid, rng, pvid):
    state_type = pvid % 4  # video_ids, topic_counts, position_tracker, diversity
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 40)
    state_off = rng.randint(0, 200)

    if state_type == 0:  # video_ids tracking
        c += f"  // Video ID state tracking\n"
        c += f"  int64_t currentId = c->queryKeys[0 % c->numKeys];\n"
        num_recent = rng.randint(5, 20)
        c += f"  int matchCount = 0;\n"
        c += f"  for (int i = 0; i < {num_recent}; ++i) {{\n"
        c += f"    int64_t prevId = static_cast<int64_t>(\n"
        c += f"      c->structData[({state_off} + i) % c->structSize] * 1e6f);\n"
        c += f"    if (prevId == currentId) matchCount++;\n"
        c += f"  }}\n"
        c += yd(fi_base, "static_cast<float>(matchCount)")
        c += f"  if (matchCount > 0) {{\n"
        c += f"    float _mc_f = static_cast<float>(matchCount);\n"
        c += f"    int32_t _mc_bits = *reinterpret_cast<int32_t*>(&_mc_f);\n"
        c += f"    _mc_bits = 0x7EF311C2 - _mc_bits;\n"
        c += f"    float _inv_mc = *reinterpret_cast<float*>(&_mc_bits);\n"
        c += yd(fi_base + 1, f"_inv_mc")
        c += f"  }}\n"
    elif state_type == 1:  # topic counts
        c += f"  // Topic count state\n"
        num_topics = rng.randint(5, 15)
        c += f"  for (int t = 0; t < {num_topics}; ++t) {{\n"
        c += f"    float count = c->structData[({state_off} + t) % c->structSize];\n"
        thr = rng.uniform(0.1, 1.0)
        c += f"    if (count > {thr:.4f}f) {{\n"
        c += f"      float score = count * {rng.uniform(0.1, 2.0):.4f}f;\n"
        c += yd(fi_base, "score")
        c += f"    }}\n"
        c += f"  }}\n"
    elif state_type == 2:  # position tracker
        c += f"  // Position tracking state\n"
        c += f"  float position = {sr(state_off)};\n"
        c += f"  float prevPosition = {sr(state_off + 1)};\n"
        c += f"  float delta = position - prevPosition;\n"
        c += yd(fi_base, "position")
        c += yd(fi_base + 1, "delta")
        c += f"  if (delta > 0.0f) {{\n"
        c += yd(fi_base + 2, f"static_cast<float>(31 - __builtin_clz(static_cast<uint32_t>(delta * 1000.0f + 1.0f)))")
        c += f"  }} else if (delta < 0.0f) {{\n"
        c += yd(fi_base + 3, f"static_cast<float>(31 - __builtin_clz(static_cast<uint32_t>(-delta * 1000.0f + 1.0f)))")
        c += f"  }}\n"
    else:  # diversity counter
        c += f"  // Diversity counter state\n"
        num_categories = rng.randint(3, 10)
        c += f"  float counts[{num_categories}] = {{}};\n"
        c += f"  for (int i = 0; i < {rng.randint(10, 30)}; ++i) {{\n"
        c += f"    int cat = static_cast<int>(c->structData[({state_off} + i) % c->structSize]"
        c += f" * {num_categories}.0f) % {num_categories};\n"
        c += f"    counts[cat] += 1.0f;\n"
        c += f"  }}\n"
        c += f"  float entropy = 0.0f;\n"
        c += f"  float total = 0.0f;\n"
        c += f"  for (int i = 0; i < {num_categories}; ++i) total += counts[i];\n"
        c += f"  if (total > 0.0f) {{\n"
        c += f"    float _tot_d = total + 1e-10f;\n"
        c += f"    int32_t _tot_b = *reinterpret_cast<int32_t*>(&_tot_d);\n"
        c += f"    _tot_b = 0x7EF311C2 - _tot_b;\n"
        c += f"    float _inv_tot = *reinterpret_cast<float*>(&_tot_b);\n"
        c += f"    for (int i = 0; i < {num_categories}; ++i) {{\n"
        c += f"      float p = counts[i] * _inv_tot;\n"
        c += f"      if (p > 0.0f) {{\n"
        c += f"        uint32_t _pi = static_cast<uint32_t>(p * 65536.0f + 1);\n"
        c += f"        entropy += p * static_cast<float>(31 - __builtin_clz(_pi));\n"
        c += f"      }}\n"
        c += f"    }}\n"
        c += f"  }}\n"
        c += yd(fi_base, "entropy")
        c += yd(fi_base + 1, "total")

    c += "}\n\n"
    return c


# ============================================================================
# Pattern P25: Multi-topic hash iteration (2-3 loops, 6-12 branches)
# ~55-75 lines per copy
# ============================================================================

def gen_P25(vid, cid, rng, pvid):
    max_topics = [3, 5, 10, 20][pvid % 4]
    rates_per_topic = [3, 5, 10][pvid % 3]
    has_filter = (pvid + cid) % 2 == 0
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 20)
    topic_off = rng.randint(0, 200)
    tidx = pvid % 4
    # Topic iteration
    c += f"  for (int t = 0; t < {max_topics}; ++t) {{\n"
    c += f"    int64_t topicId = static_cast<int64_t>(\n"
    c += f"      c->structData[({topic_off} + t) % c->structSize] * 1e5f);\n"
    if has_filter:
        thr = rng.uniform(100.0, 10000.0)
        c += f"    if (topicId < {int(thr)}LL) continue;\n"
    c += f"    auto it = c->tables[{tidx}].find(topicId);\n"
    c += f"    if (it == c->tables[{tidx}].end()) continue;\n"
    c += f"    float topicBase = it->second;\n"
    # Per-topic rate loop
    rate_off = rng.randint(0, 200)
    sc = rng.uniform(0.3, 2.0)
    c += f"    for (int r = 0; r < {rates_per_topic}; ++r) {{\n"
    c += f"      float rate = topicBase * c->structData[({rate_off} + t * {rates_per_topic}"
    c += f" + r) % c->structSize] * {sc:.4f}f;\n"
    c += f"      if (rate == 0.0f || !std::isfinite(rate)) continue;\n"
    c += f"      int fi = ({fi_base} + t * {rates_per_topic} + r) % c->numFeatures;\n"
    c += f"      if (fi >= 0 && fi < (int)c->example->idScoreLists.size())\n"
    c += f"        c->example->idScoreLists[fi].emplace_back(\n"
    c += f"          IdScorePair{{topicId * {rates_per_topic} + r, rate}});\n"
    c += f"    }}\n"
    c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P26: Delegated dynamic feature generation (0 loops, 1-3 branches)
# Thin wrapper that delegates to external holder — tiny function body
# ~10-18 lines per copy
# ============================================================================

def gen_P26(vid, cid, rng, pvid):
    num_checks = 1 + (pvid % 3)  # 1-3 pre-checks
    holder_type = pvid % 2
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 40)
    # Holder lookup (simulated via struct data)
    holder_off = rng.randint(0, 255)
    c += f"  float holderVal = {sr(holder_off)};\n"
    # Pre-check branches
    for chk in range(num_checks):
        chk_off = rng.randint(0, 255)
        thr = rng.uniform(0.0, 0.3)
        c += f"  if ({sr(chk_off)} < {thr:.4f}f) return;\n"
    # Delegation: the holder "converts all" by writing a batch of features
    # This is a tiny function — the key is it calls into a different code path
    if holder_type == 0:
        num_delegate = rng.randint(3, 8)
        c += f"  // convertAll delegation\n"
        for d in range(num_delegate):
            off = rng.randint(0, 255)
            fi = fi_base + d
            sc = rng.uniform(0.5, 2.0)
            c += f"  {{ float dv = holderVal * {sr(off)} * {sc:.4f}f;\n"
            c += yd(fi, "dv")
            c += f"  }}\n"
    else:
        # Alternative holder: forward sparse features
        num_fwd = rng.randint(2, 5)
        c += f"  // forwardAll delegation\n"
        for d in range(num_fwd):
            off = rng.randint(0, 255)
            fi = rng.randint(0, 49)
            c += f"  {{ int64_t id = static_cast<int64_t>({sr(off)} * 1e4f);\n"
            c += yi(fi, "id", f"holderVal * {rng.uniform(0.5, 2.0):.4f}f")
            c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern P27: Multi-category hierarchical FIH (2-3 loops, 8-15 branches)
# Dynamic key computation + category iteration + mobile/desktop paths
# ~70-100 lines per copy
# ============================================================================

def gen_P27(vid, cid, rng, pvid):
    num_categories = [3, 5, 7, 8][pvid % 4]
    is_mobile = (pvid + cid) % 2 == 0
    has_aggregate = (pvid % 3) != 2
    c = fhdr(vid, cid)
    fi_base = rng.randint(0, 15)
    # Dynamic key computation (unique to P27)
    c += f"  // computeFIHKeys: dynamic key generation\n"
    c += f"  int64_t catKeys[{num_categories}];\n"
    for cat in range(num_categories):
        ki = rng.randint(0, 9)
        mix0 = rng.getrandbits(48)
        mix1 = rng.getrandbits(32)
        c += f"  catKeys[{cat}] = (c->queryKeys[{ki} % c->numKeys] ^ 0x{mix0:012x}LL)"
        c += f" * 0x{mix1:08x}ULL;\n"
        c += f"  catKeys[{cat}] = (catKeys[{cat}] >> 11) ^ (catKeys[{cat}] << 3);\n"
    # Category iteration
    tidx = pvid % 4
    rates_per_cat = rng.randint(3, 8)
    c += f"  for (int cat = 0; cat < {num_categories}; ++cat) {{\n"
    c += f"    auto it = c->tables[{tidx}].find(catKeys[cat]);\n"
    c += f"    if (it == c->tables[{tidx}].end()) continue;\n"
    c += f"    float catBase = it->second;\n"
    # Mobile vs desktop conditional paths
    if is_mobile:
        c += f"    // mobile path\n"
        for r in range(rates_per_cat):
            off = rng.randint(0, 255)
            sc = rng.uniform(0.3, 2.0)
            fi = fi_base + r
            c += f"    {{ float mv = catBase * {sr(off)} * {sc:.4f}f;\n"
            transform = rng.choice(["mv", "(mv > 3.0f ? 1.0f : (mv < -3.0f ? -1.0f : mv * 0.33f))", "(mv > 0.0f ? mv : -mv)"])
            c += f"      float t = {transform};\n"
            c += yi(fi, f"catKeys[cat]", "t")
            c += f"    }}\n"
    else:
        c += f"    // desktop path\n"
        for r in range(rates_per_cat):
            off = rng.randint(0, 255)
            sc = rng.uniform(0.3, 2.0)
            fi = fi_base + rates_per_cat + r
            c += f"    {{ float dv = catBase * {sr(off)} * {sc:.4f}f;\n"
            # Desktop path uses different computation
            c += f"      float t = dv * dv * 0.01f;\n"
            c += f"      if (t > 0.001f) {{\n"
            c += yd(fi, "t")
            c += f"      }}\n"
            c += f"    }}\n"
    c += f"  }}\n"
    # Aggregate "all categories" pass
    if has_aggregate:
        c += f"  // all-categories aggregate\n"
        c += f"  float aggSum = 0.0f;\n"
        c += f"  int aggCount = 0;\n"
        c += f"  for (int cat = 0; cat < {num_categories}; ++cat) {{\n"
        c += f"    auto it = c->tables[{tidx}].find(catKeys[cat]);\n"
        c += f"    if (it != c->tables[{tidx}].end()) {{\n"
        c += f"      aggSum += it->second;\n"
        c += f"      aggCount++;\n"
        c += f"    }}\n"
        c += f"  }}\n"
        c += f"  if (aggCount > 0) {{\n"
        fi_agg = rng.randint(0, 49)
        c += f"    float _ac_f = static_cast<float>(aggCount);\n"
        c += f"    int32_t _ac_bits = *reinterpret_cast<int32_t*>(&_ac_f);\n"
        c += f"    _ac_bits = 0x7EF311C2 - _ac_bits;\n"
        c += f"    float _inv_ac = *reinterpret_cast<float*>(&_ac_bits);\n"
        c += yd(fi_agg, "aggSum * _inv_ac")
        fi_cnt = rng.randint(0, 49)
        c += yd(fi_cnt, "static_cast<float>(aggCount)")
        c += f"  }}\n"
    c += "}\n\n"
    return c


# ============================================================================
# Pattern generator dispatch table
# ============================================================================

PATTERN_GENERATORS = {
    "P01": gen_P01, "P02": gen_P02, "P03": gen_P03, "P04": gen_P04,
    "P05": gen_P05, "P06": gen_P06, "P07": gen_P07, "P08": gen_P08,
    "P09": gen_P09, "P10": gen_P10, "P11": gen_P11, "P12": gen_P12,
    "P13": gen_P13, "P14": gen_P14, "P15": gen_P15, "P16": gen_P16,
    "P17": gen_P17, "P18": gen_P18, "P19": gen_P19, "P20": gen_P20,
    "P21": gen_P21, "P22": gen_P22, "P23": gen_P23, "P24": gen_P24,
    "P25": gen_P25, "P26": gen_P26, "P27": gen_P27,
}


# ============================================================================
# Variant spec generation
# ============================================================================

def generate_all_variants() -> List[VariantSpec]:
    specs = []
    global_idx = 0
    for pattern, count, struct_size, table_size in PATTERN_DISTRIBUTION:
        for i in range(count):
            seed = RANDOM_SEED + global_idx * 7919 + i * 31
            spec = VariantSpec(
                name=f"ExtractorV_{pattern}_{global_idx:04d}",
                pattern=pattern,
                variant_idx=global_idx,
                pattern_variant_idx=i,
                seed=seed,
                struct_size=struct_size,
                table_size=table_size,
            )
            specs.append(spec)
            global_idx += 1
    return specs


# ============================================================================
# File generators (dispatch.h, copies, variants, registry, cmake)
# ============================================================================

def generate_dispatch_header(output_dir, num_variants, copies_per_variant):
    path = os.path.join(output_dir, "dispatch.h")
    with open(path, "w") as f:
        f.write(COPYRIGHT)
        f.write(f"""
#pragma once

#include "../FeatureTypes.h"
#include "mock_hash_table.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dcperf {{
namespace feature_extractors {{
namespace generated {{

struct CopyContext {{
  std::unordered_map<int64_t, float>* tables;  // array of 4 tables
  float* structData;
  int structSize;
  MockFeatureExample* example;
  const MockFeature* features;
  int numFeatures;
  const int64_t* queryKeys;
  int numKeys;
  // Mock hash tables for deep-call-chain lookups (mimics F14/QuickHash)
  mock_hash::MockHashTable* hashTables;  // array of 4 mock hash tables
  int numHashTables;
  // Phase 3 (Plan): Story content from Silesia corpus
  const uint8_t* storyContent = nullptr;
  int storyContentLength = 0;
}};

using CopyFn = void(*)(CopyContext*);

constexpr int kCopiesPerVariant = {copies_per_variant};

""")
        for vid in range(num_variants):
            f.write(f"void CopiesInit_{vid:04d}(CopyFn* arr);\n")
        f.write("""
} // namespace generated
} // namespace feature_extractors
} // namespace dcperf
""")


def generate_copies_part(spec, copies_per_variant, output_dir):
    copies_dir = os.path.join(output_dir, "copies")
    filepath = os.path.join(copies_dir, f"copies_part_{spec.variant_idx:04d}.cpp")
    gen_fn = PATTERN_GENERATORS[spec.pattern]

    with open(filepath, "w") as f:
        f.write(COPYRIGHT)
        f.write("\n")
        f.write('#include "../dispatch.h"\n')
        f.write('#include "../extractor_helpers.h"\n')
        f.write("#include <cmath>\n")
        f.write("#include <cstdint>\n")
        f.write("\n")
        f.write("namespace dcperf {\n")
        f.write("namespace feature_extractors {\n")
        f.write("namespace generated {\n\n")

        for cid in range(copies_per_variant):
            rng = random.Random(spec.seed * 1000003 + cid * 7)
            f.write(gen_fn(spec.variant_idx, cid, rng, spec.pattern_variant_idx))

        # CopiesInit function
        f.write(f"void CopiesInit_{spec.variant_idx:04d}(CopyFn* arr) {{\n")
        for cid in range(copies_per_variant):
            f.write(f"  arr[{cid}] = vc_{spec.variant_idx:04d}_{cid:04d};\n")
        f.write("}\n\n")

        f.write("} // namespace generated\n")
        f.write("} // namespace feature_extractors\n")
        f.write("} // namespace dcperf\n")


def generate_variant_class(spec, copies_per_variant):
    lines = []
    lines.append(f"class {spec.name} : public ArchetypeBase {{")
    lines.append(f"  static constexpr uint64_t kSeed = {spec.seed}ULL;")
    lines.append(f"")
    lines.append(f" public:")
    lines.append(f"  void initializeImpl(int complexity) override {{")
    lines.append(f"    uint64_t state = kSeed + complexity;")
    lines.append(f"    structSize_ = {spec.struct_size};")
    lines.append(f"    structData_ = new float[structSize_];")
    lines.append(f"    for (int i = 0; i < structSize_; ++i)")
    lines.append(f"      structData_[i] = randomFloat(state);")
    lines.append(f"    for (int t = 0; t < 4; ++t)")
    lines.append(f"      for (int i = 0; i < {spec.table_size}; ++i)")
    lines.append(f"        tables_[t][randomInt64(state)] = randomFloat(state);")
    lines.append(f"    for (int t = 0; t < 4; ++t)")
    lines.append(f"      hashTables_[t].populate(64, kSeed + t + complexity);")
    lines.append(f"    CopiesInit_{spec.variant_idx:04d}(copies_);")
    lines.append(f"    rngState_ = kSeed;")
    lines.append(f"  }}")
    lines.append(f"")
    lines.append(f"  ~{spec.name}() override {{ delete[] structData_; }}")
    lines.append(f"")
    lines.append(f"  __attribute__((noinline)) void extractImpl(")
    lines.append(f"      MockFeatureExample& example,")
    lines.append(f"      const std::vector<MockFeature>& features,")
    lines.append(f"      const std::vector<int64_t>& queryKeys) override {{")
    lines.append(f"    CopyContext ctx;")
    lines.append(f"    ctx.tables = tables_;")
    lines.append(f"    ctx.structData = structData_;")
    lines.append(f"    ctx.structSize = structSize_;")
    lines.append(f"    ctx.example = &example;")
    lines.append(f"    ctx.features = features.data();")
    lines.append(f"    ctx.numFeatures = static_cast<int>(features.size());")
    lines.append(f"    ctx.queryKeys = queryKeys.data();")
    lines.append(f"    ctx.numKeys = static_cast<int>(queryKeys.size());")
    lines.append(f"    ctx.hashTables = hashTables_;")
    lines.append(f"    ctx.numHashTables = 4;")
    lines.append(f"    int idx = static_cast<int>(rngState_ % kCopiesPerVariant);")
    lines.append(f"    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;")
    lines.append(f"    copies_[idx](&ctx);")
    lines.append(f"  }}")
    lines.append(f"")
    lines.append(f"  std::string name() const override {{ return \"{spec.name}\"; }}")
    lines.append(f"")
    lines.append(f" private:")
    lines.append(f"  float* structData_ = nullptr;")
    lines.append(f"  int structSize_ = 0;")
    lines.append(f"  std::unordered_map<int64_t, float> tables_[4];")
    lines.append(f"  dcperf::mock_hash::MockHashTable hashTables_[4];")
    lines.append(f"  CopyFn copies_[kCopiesPerVariant];")
    lines.append(f"  uint64_t rngState_ = 0;")
    lines.append(f"}};")
    return "\n".join(lines)


def generate_batch_files(specs, output_dir, variants_per_batch, copies_per_variant):
    variants_dir = os.path.join(output_dir, "variants")
    os.makedirs(variants_dir, exist_ok=True)
    batch_files = []
    batch_idx = 0
    for start in range(0, len(specs), variants_per_batch):
        batch = specs[start:start + variants_per_batch]
        filename = f"variants_batch_{batch_idx:03d}.cpp"
        filepath = os.path.join(variants_dir, filename)
        content = [COPYRIGHT, ""]
        content.append('#include "../archetype_templates/ArchetypeBase.h"')
        content.append('#include "../archetype_templates/ArchetypeUtils.h"')
        content.append('#include "../dispatch.h"')
        content.append('#include "../mock_hash_table.h"')
        content.append("#include <cmath>")
        content.append("#include <memory>")
        content.append("#include <string>")
        content.append("#include <unordered_map>")
        content.append("#include <vector>")
        content.append("")
        content.append("namespace dcperf {")
        content.append("namespace feature_extractors {")
        content.append("namespace generated {")
        content.append("")
        for spec in batch:
            content.append(generate_variant_class(spec, copies_per_variant))
            content.append("")
        func_name = f"createBatch{batch_idx:03d}"
        content.append(
            f"void {func_name}("
            f"std::vector<std::unique_ptr<FeatureExtractorBase>>& v) {{"
        )
        for spec in batch:
            content.append(f"  v.push_back(std::make_unique<{spec.name}>());")
        content.append("}")
        content.append("")
        content.append("} // namespace generated")
        content.append("} // namespace feature_extractors")
        content.append("} // namespace dcperf")
        content.append("")
        with open(filepath, "w") as f:
            f.write("\n".join(content))
        batch_files.append(filename)
        batch_idx += 1
    return batch_files


def generate_registry(specs, num_batches, output_dir):
    num_variants = len(specs)
    header_path = os.path.join(output_dir, "registry.h")
    with open(header_path, "w") as f:
        f.write(COPYRIGHT)
        f.write(f"""
#pragma once

#include "../FeatureExtractorBase.h"
#include "dispatch.h"
#include <memory>
#include <vector>

namespace dcperf {{
namespace feature_extractors {{
namespace generated {{

std::vector<std::unique_ptr<FeatureExtractorBase>>
createGeneratedExtractors();

constexpr int kNumGeneratedVariants = {num_variants};

// Collect all copy function pointers into a flat vector for sequential dispatch
void getAllCopyFunctions(std::vector<CopyFn>& out);

}} // namespace generated
}} // namespace feature_extractors
}} // namespace dcperf
""")

    cpp_path = os.path.join(output_dir, "registry.cpp")
    lines = [COPYRIGHT, ""]
    lines.append('#include "registry.h"')
    lines.append('#include "dispatch.h"')
    lines.append("")
    lines.append("namespace dcperf {")
    lines.append("namespace feature_extractors {")
    lines.append("namespace generated {")
    lines.append("")
    for i in range(num_batches):
        lines.append(
            f"void createBatch{i:03d}("
            f"std::vector<std::unique_ptr<FeatureExtractorBase>>& v);"
        )
    lines.append("")
    lines.append("std::vector<std::unique_ptr<FeatureExtractorBase>>")
    lines.append("createGeneratedExtractors() {")
    lines.append("  std::vector<std::unique_ptr<FeatureExtractorBase>> extractors;")
    lines.append(f"  extractors.reserve({num_variants});")
    lines.append("")
    for i in range(num_batches):
        lines.append(f"  createBatch{i:03d}(extractors);")
    lines.append("")
    lines.append("  return extractors;")
    lines.append("}")
    lines.append("")
    lines.append(f"void getAllCopyFunctions(std::vector<CopyFn>& out) {{")
    lines.append(f"  constexpr int N = {num_variants};")
    lines.append(f"  out.resize(N * kCopiesPerVariant);")
    lines.append(f"  using InitFn = void(*)(CopyFn*);")
    lines.append(f"  static const InitFn inits[N] = {{")
    for i in range(num_variants):
        comma = "," if i < num_variants - 1 else ""
        lines.append(f"    CopiesInit_{i:04d}{comma}")
    lines.append(f"  }};")
    lines.append(f"  for (int v = 0; v < N; ++v) {{")
    lines.append(f"    inits[v](out.data() + v * kCopiesPerVariant);")
    lines.append(f"  }}")
    lines.append(f"}}")
    lines.append("")
    lines.append("} // namespace generated")
    lines.append("} // namespace feature_extractors")
    lines.append("} // namespace dcperf")
    lines.append("")
    with open(cpp_path, "w") as f:
        f.write("\n".join(lines))


def generate_cmake_fragment(batch_files, num_variants, output_dir):
    cmake_path = os.path.join(output_dir, "generated_sources.cmake")
    lines = [
        "# Auto-generated by generate_extractors.py -- do not edit",
        "",
        "set(GENERATED_EXTRACTOR_SOURCES",
        "  ${CMAKE_CURRENT_SOURCE_DIR}/feature_extractors/generated/registry.cpp",
    ]
    for bf in batch_files:
        lines.append(
            f"  ${{CMAKE_CURRENT_SOURCE_DIR}}/feature_extractors/generated/variants/{bf}"
        )
    for vid in range(num_variants):
        lines.append(
            f"  ${{CMAKE_CURRENT_SOURCE_DIR}}/feature_extractors/generated/"
            f"copies/copies_part_{vid:04d}.cpp"
        )
    lines.append(")")
    lines.append("")
    with open(cmake_path, "w") as f:
        f.write("\n".join(lines))


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Generate diverse feature extractor variants (25 patterns)"
    )
    parser.add_argument(
        "--output-dir",
        default=os.path.dirname(os.path.abspath(__file__)),
        help="Output directory for generated files",
    )
    parser.add_argument(
        "--variants-per-batch",
        type=int,
        default=10,
        help="Number of variant classes per batch .cpp file",
    )
    parser.add_argument(
        "--copies-per-variant",
        type=int,
        default=150,
        help="Number of copy functions per variant (default 150)",
    )
    args = parser.parse_args()

    t0 = time.time()
    print(f"Output directory: {args.output_dir}")
    print(f"Variants per batch: {args.variants_per_batch}")
    print(f"Copies per variant: {args.copies_per_variant}")
    print()

    copies_dir = os.path.join(args.output_dir, "copies")
    os.makedirs(copies_dir, exist_ok=True)

    specs = generate_all_variants()
    total = len(specs)
    total_copies = total * args.copies_per_variant
    print(f"Total variants: {total}")
    print(f"Total copy functions: {total_copies:,}")
    print()

    # Pattern breakdown
    pattern_counts = {}
    for spec in specs:
        pattern_counts[spec.pattern] = pattern_counts.get(spec.pattern, 0) + 1
    for pat, cnt in sorted(pattern_counts.items()):
        print(f"  {pat}: {cnt} variants")
    print()

    # Generate dispatch header
    generate_dispatch_header(args.output_dir, total, args.copies_per_variant)
    print("Generated dispatch.h")

    # Generate copies part files
    print(f"Generating {total} copies_part files...", end="", flush=True)
    for i, spec in enumerate(specs):
        generate_copies_part(spec, args.copies_per_variant, args.output_dir)
        if (i + 1) % 50 == 0:
            print(f" {i + 1}", end="", flush=True)
    print(f" done")

    # Generate variant batch files
    batch_files = generate_batch_files(
        specs, args.output_dir, args.variants_per_batch,
        args.copies_per_variant)
    print(f"Generated {len(batch_files)} variant batch files")

    # Generate registry
    generate_registry(specs, len(batch_files), args.output_dir)
    print("Generated registry.h and registry.cpp")

    # Generate cmake fragment
    generate_cmake_fragment(batch_files, total, args.output_dir)
    print("Generated generated_sources.cmake")

    elapsed = time.time() - t0
    print(f"\nGeneration time: {elapsed:.1f}s")
    print(f"Estimated unique functions: {total_copies:,}")
    print(f"\nDone: {total} variants x {args.copies_per_variant} copies"
          f" = {total_copies:,} unique functions across 25 patterns")


if __name__ == "__main__":
    main()
