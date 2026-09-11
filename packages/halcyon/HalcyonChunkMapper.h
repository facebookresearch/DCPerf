// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Forward-declare RocksDB so includers of this header (the engine, main.cpp,
// the thrift handler) do not need RocksDB headers on their include path. The
// destructor is defined out-of-line in the .cpp so unique_ptr<rocksdb::DB>
// works with the incomplete type here.
namespace rocksdb {
class DB;
class Cache;
} // namespace rocksdb

namespace facebook::halcyon {

/// Benchmark chunk identifier. A dense 64-bit key space lets the read path pick
/// an existing key cheaply (rand64(maxChunkId)). HyperNode uses a 128-bit
/// __uint128_t id; halcyon narrows it to uint64_t -- enough keys for a
/// benchmark and a fixed 8-byte RocksDB key.
using ChunkId = uint64_t;

/// Physical location of a chunk's data in halcyon's flat-file model. Mirrors
/// the role of HyperNode's serialized ChunkInfo (partition + LBA ranges) but
/// collapsed to a single contiguous extent in one file. Serialized as a fixed
/// 24-byte little-endian record (see serialize/deserialize).
struct ChunkLocation {
  uint32_t fileIndex = 0; ///< index into the reactor's fds_
  uint32_t length = 0; ///< on-disk byte length (kSectorSize-aligned)
  uint64_t offset = 0; ///< byte offset within the file (kSectorSize-aligned)
  uint32_t checksum = 0; ///< optional folly::crc32 of the data (0 if unused)
  uint32_t reserved = 0; ///< padding / future use
};

/// Fixed on-disk size of a serialized ChunkLocation.
inline constexpr size_t kChunkLocationSize = 24;

/// Appends the little-endian serialization of `loc` to `out`.
void serialize(const ChunkLocation& loc, std::string& out);
/// Parses a serialized ChunkLocation. Returns false if the input is not exactly
/// kChunkLocationSize bytes.
bool deserialize(std::string_view in, ChunkLocation& out);

/// RocksDB-backed chunk id -> physical location index, mirroring the metadata
/// approach HyperNode's ChunkMapper/RocksKVStore uses, but as a thin
/// halcyon-local component that depends only on RocksDB. One instance owns one
/// RocksDB database (one per reactor / mount point in the engine).
///
/// Not thread-safe across instances sharing a path; intended to be owned by a
/// single reactor thread. Follows halcyon's local convention of throwing
/// std::runtime_error on unrecoverable errors (NOT HyperNode's util::Status).
class HalcyonChunkMapper {
 public:
  struct Options {
    /// Use O_DIRECT reads in RocksDB (set to match the benchmark's doDirectIo).
    bool doDirectReads = true;
    /// Block cache size in bytes. Ignored if sharedBlockCache is non-null.
    size_t blockCacheBytes = 128ul << 20;
    /// Optional block cache shared across HalcyonChunkMapper instances (one
    /// per mount). When non-null, all mappers use this single LRU instead of
    /// each allocating its own — mirrors HyperNode's
    /// chunkMapperConfig.kvStoreConfig.rocksKVStoreConfig.sharedBlockCacheSizeMiB
    /// which is one cache split across every mount's column family.
    /// Sharing amortizes cache capacity across mounts and improves hit rate
    /// on hot chunks that live on any drive.
    std::shared_ptr<rocksdb::Cache> sharedBlockCache;
    /// fsync the WAL on every mapping write. Off by default: the benchmark
    /// measures data durability (O_DIRECT|O_SYNC on the data file), not mapping
    /// durability.
    bool syncOnPut = false;
  };

  /// Opens (creating if missing) a RocksDB at `path`. Throws std::runtime_error
  /// on open failure.
  HalcyonChunkMapper(std::string path, const Options& opts);
  ~HalcyonChunkMapper();

  HalcyonChunkMapper(const HalcyonChunkMapper&) = delete;
  HalcyonChunkMapper& operator=(const HalcyonChunkMapper&) = delete;

  /// Hot-path read: looks up the location for `id`. Returns false on
  /// miss/error.
  bool lookup(ChunkId id, ChunkLocation& out) const;

  /// Batched read (documented future hot-path option via MultiGet). `out` and
  /// `found` are resized to ids.size(); found[i] indicates a hit for ids[i].
  void multiLookup(
      const std::vector<ChunkId>& ids,
      std::vector<ChunkLocation>& out,
      std::vector<bool>& found) const;

  /// Hot-path write: persists one mapping. Throws on write error.
  void put(ChunkId id, const ChunkLocation& loc);

  /// Bulk population: persists many mappings in a single WriteBatch.
  void putBatch(const std::vector<std::pair<ChunkId, ChunkLocation>>& entries);

  /// Forces the write-ahead log durable (fsync). Lets callers write mappings
  /// with sync=false (the default) and amortize one fsync across many writes --
  /// a batched put() / putBatch() followed by a single syncWal(). Throws on
  /// error.
  void syncWal();

  /// Highest populated id + 1; the read path selects keys in [0, maxChunkId()).
  ChunkId maxChunkId() const {
    return maxChunkId_;
  }
  /// Sets and persists the max chunk id so a later read-only run knows the key
  /// range.
  void setMaxChunkId(ChunkId n);

  /// Picks a random key in [0, maxId). Returns 0 if maxId == 0.
  static ChunkId randomExistingKey(ChunkId maxId);

  /// Picks a read key in [0, maxId) with injected temporal locality. With
  /// probability `locality` the key is drawn from the most-recent `window` ids
  /// ([maxId-window, maxId)) -- the workload-access-locality signal an LRU
  /// cache exploits, modeling recently-written/read chunks being re-read.
  /// Otherwise (and whenever locality <= 0, window == 0, or window >= maxId) it
  /// falls back to the uniform randomExistingKey. Returns 0 if maxId == 0.
  static ChunkId randomLocalKey(ChunkId maxId, double locality, ChunkId window);

 private:
  void loadMaxChunkId();

  std::unique_ptr<rocksdb::DB> db_;
  bool syncOnPut_ = false;
  ChunkId maxChunkId_ = 0;
};

} // namespace facebook::halcyon
