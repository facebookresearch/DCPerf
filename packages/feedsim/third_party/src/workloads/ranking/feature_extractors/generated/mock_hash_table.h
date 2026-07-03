// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Mock hash table with deep call chains mimicking F14HashMap/QuickHashTable.
// Production hash lookups go through 5-7 levels:
//   operator[] -> emplaceKeyArgs -> emplaceKeyArgsWithToken -> findImpl
//     -> probeChunk -> tagMatchIter -> compareKey
// This mock replicates that call depth to generate realistic I-cache pressure.

#pragma once

#include <cstdint>
#include <cstring>

namespace dcperf {
namespace mock_hash {

// Tag byte for chunk-based probing (like F14's tag matching)
using Tag = uint8_t;

static constexpr int kChunkSize = 14;  // F14 uses 14 slots per chunk
static constexpr int kNumChunks = 128; // Fixed chunk count for mock
static constexpr int kCapacity = kChunkSize * kNumChunks;
static constexpr Tag kEmpty = 0;
static constexpr Tag kTombstone = 1;

struct Entry {
  int64_t key;
  float value;
};

struct Chunk {
  Tag tags[kChunkSize];
  Entry entries[kChunkSize];
};

// Forward declarations for non-inline helper functions.
// Each is __attribute__((noinline)) to force real function calls,
// mimicking the depth of F14Table::findImpl -> tagMatchIter chain.

struct MockHashTable {
  Chunk chunks[kNumChunks];
  int size_ = 0;

  void init() {
    memset(chunks, 0, sizeof(chunks));
    size_ = 0;
  }

  // Pre-populate with data for lookups
  void populate(int count, uint64_t seed) {
    uint64_t state = seed;
    for (int i = 0; i < count && size_ < kCapacity; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      int64_t key = static_cast<int64_t>(state >> 16);
      float val = static_cast<float>(state & 0xFFFF) / 65535.0f;
      insert(key, val);
    }
  }

  // ======================================================================
  // Level 1: operator[] — entry point (like QuickHashMap::operator[])
  // ======================================================================
  __attribute__((noinline))
  float& operator[](int64_t key) {
    return emplaceKeyArgs(key);
  }

  // ======================================================================
  // Level 2: emplaceKeyArgs (like QuickHashTable::emplaceKeyArgs)
  // ======================================================================
  __attribute__((noinline))
  float& emplaceKeyArgs(int64_t key) {
    Tag tag = computeTag(key);
    int chunk_idx = computeChunkIndex(key);
    return emplaceKeyArgsWithToken(key, tag, chunk_idx);
  }

  // ======================================================================
  // Level 3: emplaceKeyArgsWithToken
  // ======================================================================
  __attribute__((noinline))
  float& emplaceKeyArgsWithToken(int64_t key, Tag tag, int chunk_idx) {
    int slot = findImpl(key, tag, chunk_idx);
    if (slot >= 0) {
      Chunk& c = chunks[chunk_idx];
      return c.entries[slot].value;
    }
    // Insert new entry
    return insertAtFirstEmpty(key, tag, chunk_idx);
  }

  // ======================================================================
  // Level 4: findImpl (like F14Table::findImpl / QuickHashTable::findImpl)
  // ======================================================================
  __attribute__((noinline))
  int findImpl(int64_t key, Tag tag, int chunk_idx) {
    Chunk& c = chunks[chunk_idx % kNumChunks];
    int match_pos = tagMatchIter(c, tag);
    if (match_pos >= 0) {
      if (compareKey(c, match_pos, key)) {
        return match_pos;
      }
      // Continue probing (linear probe within chunk)
      return probeChunk(c, key, tag, match_pos + 1);
    }
    return -1;
  }

  // ======================================================================
  // Level 5: tagMatchIter (like F14Chunk::tagMatchIter)
  // Scans tag array for matching tag byte
  // ======================================================================
  __attribute__((noinline))
  int tagMatchIter(const Chunk& chunk, Tag tag) {
    for (int i = 0; i < kChunkSize; ++i) {
      if (chunk.tags[i] == tag) {
        return i;
      }
    }
    return -1;
  }

  // ======================================================================
  // Level 6: probeChunk — continue linear probing within chunk
  // ======================================================================
  __attribute__((noinline))
  int probeChunk(const Chunk& chunk, int64_t key, Tag tag, int start) {
    for (int i = start; i < kChunkSize; ++i) {
      if (chunk.tags[i] == kEmpty) return -1;
      if (chunk.tags[i] == tag && compareKey(chunk, i, key)) {
        return i;
      }
    }
    return -1;
  }

  // ======================================================================
  // Level 7: compareKey — key equality check
  // ======================================================================
  __attribute__((noinline))
  bool compareKey(const Chunk& chunk, int slot, int64_t key) {
    return chunk.entries[slot].key == key;
  }

  // ======================================================================
  // find() — read-only lookup returning pointer (like F14Map::find)
  // Now uses the same deep call chain as the write path:
  //   find -> findConstImpl -> tagMatchIterConst -> compareKeyConst
  //                         -> probeChunkConst
  // ======================================================================
  __attribute__((noinline))
  const float* find(int64_t key) const {
    Tag tag = computeTag(key);
    int chunk_idx = computeChunkIndex(key);
    return findConstImpl(key, tag, chunk_idx);
  }

  // ======================================================================
  // Level 2 (const): findConstImpl — mirrors findImpl for read path
  // ======================================================================
  __attribute__((noinline))
  const float* findConstImpl(int64_t key, Tag tag, int chunk_idx) const {
    const Chunk& c = chunks[chunk_idx % kNumChunks];
    int match_pos = tagMatchIterConst(c, tag);
    if (match_pos >= 0) {
      if (compareKeyConst(c, match_pos, key)) {
        return &c.entries[match_pos].value;
      }
      return probeChunkConst(c, key, tag, match_pos + 1);
    }
    return nullptr;
  }

  // ======================================================================
  // Level 3 (const): tagMatchIterConst — tag scan for read path
  // ======================================================================
  __attribute__((noinline))
  int tagMatchIterConst(const Chunk& chunk, Tag tag) const {
    for (int i = 0; i < kChunkSize; ++i) {
      if (chunk.tags[i] == tag) {
        return i;
      }
    }
    return -1;
  }

  // ======================================================================
  // Level 4 (const): compareKeyConst — key equality for read path
  // ======================================================================
  __attribute__((noinline))
  bool compareKeyConst(const Chunk& chunk, int slot, int64_t key) const {
    return chunk.entries[slot].key == key;
  }

  // ======================================================================
  // Level 5 (const): probeChunkConst — linear probing for read path
  // ======================================================================
  __attribute__((noinline))
  const float* probeChunkConst(const Chunk& chunk, int64_t key, Tag tag, int start) const {
    for (int i = start; i < kChunkSize; ++i) {
      if (chunk.tags[i] == kEmpty) return nullptr;
      if (chunk.tags[i] == tag && compareKeyConst(chunk, i, key)) {
        return &chunk.entries[i].value;
      }
    }
    return nullptr;
  }

  // ======================================================================
  // Helpers
  // ======================================================================
  static Tag computeTag(int64_t key) {
    uint64_t h = static_cast<uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
    Tag t = static_cast<Tag>((h >> 56) | 2);  // Avoid 0 (empty) and 1 (tombstone)
    return t;
  }

  static int computeChunkIndex(int64_t key) {
    uint64_t h = static_cast<uint64_t>(key) * 0x517CC1B727220A95ULL;
    return static_cast<int>((h >> 32) % kNumChunks);
  }

  void insert(int64_t key, float value) {
    Tag tag = computeTag(key);
    int ci = computeChunkIndex(key);
    Chunk& c = chunks[ci % kNumChunks];
    for (int i = 0; i < kChunkSize; ++i) {
      if (c.tags[i] == kEmpty || c.tags[i] == kTombstone) {
        c.tags[i] = tag;
        c.entries[i] = {key, value};
        ++size_;
        return;
      }
      if (c.tags[i] == tag && c.entries[i].key == key) {
        c.entries[i].value = value;
        return;
      }
    }
  }

  __attribute__((noinline))
  float& insertAtFirstEmpty(int64_t key, Tag tag, int chunk_idx) {
    Chunk& c = chunks[chunk_idx % kNumChunks];
    for (int i = 0; i < kChunkSize; ++i) {
      if (c.tags[i] == kEmpty || c.tags[i] == kTombstone) {
        c.tags[i] = tag;
        c.entries[i].key = key;
        c.entries[i].value = 0.0f;
        ++size_;
        return c.entries[i].value;
      }
    }
    // Overflow: use first slot (shouldn't happen with proper sizing).
    // Also update the tag so subsequent find(key) computes a matching tag.
    c.tags[0] = tag;
    c.entries[0].key = key;
    c.entries[0].value = 0.0f;
    return c.entries[0].value;
  }
};

// ======================================================================
// Multi-table lookup helpers — simulate cross-table hash joins
// (production has lookups across 2-4 hash tables per extractor)
// ======================================================================

// Level 1: High-level lookup helper
__attribute__((noinline))
inline float hashLookupWithFallback(
    MockHashTable& table, int64_t key, float fallback) {
  const float* p = table.find(key);
  return p ? *p : fallback;
}

// Cross-table join: look up in A, use result to derive key for B
__attribute__((noinline))
inline float crossTableLookup(
    MockHashTable& tableA, MockHashTable& tableB,
    int64_t key, float scale) {
  float valA = hashLookupWithFallback(tableA, key, 0.0f);
  int64_t derivedKey = static_cast<int64_t>(valA * scale) ^ key;
  return hashLookupWithFallback(tableB, derivedKey, valA);
}

// Multi-key accumulation: look up N keys and accumulate
__attribute__((noinline))
inline float multiKeyAccumulate(
    MockHashTable& table, const int64_t* keys, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) {
    sum += hashLookupWithFallback(table, keys[i], 0.0f);
  }
  return sum;
}

} // namespace mock_hash
} // namespace dcperf
