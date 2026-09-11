// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "HalcyonChunkMapper.h"

#include <cstring>
#include <stdexcept>

#include <fmt/format.h>
#include <folly/Format.h>
#include <folly/Random.h>

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

namespace facebook::halcyon {

namespace {

// Sentinel key holding the persisted max chunk id. Its length (> 8 bytes)
// guarantees it never collides with a chunk key (raw 8-byte ChunkId).
constexpr std::string_view kMaxChunkIdKey = "halcyon_max_chunk_id";

rocksdb::Slice chunkKey(const ChunkId& id) {
  return rocksdb::Slice(reinterpret_cast<const char*>(&id), sizeof(id));
}

rocksdb::Options buildOptions(const HalcyonChunkMapper::Options& opts) {
  rocksdb::Options o;
  o.create_if_missing = true;
  o.allow_concurrent_memtable_write = true;
  o.max_background_jobs = 4;
  o.use_direct_reads = opts.doDirectReads;
  o.use_direct_io_for_flush_and_compaction = opts.doDirectReads;
  o.OptimizeLevelStyleCompaction(256ul << 20);

  rocksdb::BlockBasedTableOptions t;
  // Ribbon filter + block cache mirror HyperNode's RocksKVStore CF options.
  // A single fixed 8-byte key needs no prefix extractor, so use binary-search
  // index (kHashSearch is HyperNode's choice for its prefix-scannable CFs).
  t.filter_policy.reset(rocksdb::NewRibbonFilterPolicy(8));
  t.index_type = rocksdb::BlockBasedTableOptions::kBinarySearch;
  if (opts.sharedBlockCache) {
    t.block_cache = opts.sharedBlockCache;
  } else {
    t.block_cache = rocksdb::NewLRUCache(opts.blockCacheBytes);
  }
  t.cache_index_and_filter_blocks = true;
  o.table_factory.reset(rocksdb::NewBlockBasedTableFactory(t));
  o.compression = rocksdb::kLZ4Compression;
  return o;
}

// Meta-internal RocksDB declares DB::Open taking std::unique_ptr<DB>*, the
// open-source build takes DB**. Exactly one of the two exists per build, so the
// unused arm has to be a discarded template branch rather than dead code.
template <typename DB>
rocksdb::Status openDb(
    const rocksdb::Options& opts,
    const std::string& path,
    std::unique_ptr<DB>& out) {
  if constexpr (requires { DB::Open(opts, path, &out); }) {
    return DB::Open(opts, path, &out);
  } else {
    DB* raw = nullptr;
    const rocksdb::Status s = DB::Open(opts, path, &raw);
    out.reset(raw);
    return s;
  }
}

} // namespace

void serialize(const ChunkLocation& loc, std::string& out) {
  const auto append = [&](const void* p, size_t n) {
    out.append(reinterpret_cast<const char*>(p), n);
  };
  append(&loc.fileIndex, sizeof(loc.fileIndex));
  append(&loc.length, sizeof(loc.length));
  append(&loc.offset, sizeof(loc.offset));
  append(&loc.checksum, sizeof(loc.checksum));
  append(&loc.reserved, sizeof(loc.reserved));
}

bool deserialize(std::string_view in, ChunkLocation& out) {
  if (in.size() != kChunkLocationSize) {
    return false;
  }
  const char* p = in.data();
  std::memcpy(&out.fileIndex, p, sizeof(out.fileIndex));
  p += sizeof(out.fileIndex);
  std::memcpy(&out.length, p, sizeof(out.length));
  p += sizeof(out.length);
  std::memcpy(&out.offset, p, sizeof(out.offset));
  p += sizeof(out.offset);
  std::memcpy(&out.checksum, p, sizeof(out.checksum));
  p += sizeof(out.checksum);
  std::memcpy(&out.reserved, p, sizeof(out.reserved));
  return true;
}

HalcyonChunkMapper::HalcyonChunkMapper(std::string path, const Options& opts)
    : syncOnPut_(opts.syncOnPut) {
  const rocksdb::Options dbOpts = buildOptions(opts);
  const rocksdb::Status s = openDb(dbOpts, path, db_);
  if (!s.ok()) {
    throw std::runtime_error(
        fmt::format("Failed to open chunk map at {}: {}", path, s.ToString()));
  }
  loadMaxChunkId();
}

HalcyonChunkMapper::~HalcyonChunkMapper() = default;

void HalcyonChunkMapper::loadMaxChunkId() {
  std::string value;
  const rocksdb::Status s = db_->Get(
      rocksdb::ReadOptions(),
      rocksdb::Slice(kMaxChunkIdKey.data(), kMaxChunkIdKey.size()),
      &value);
  if (s.ok() && value.size() == sizeof(ChunkId)) {
    std::memcpy(&maxChunkId_, value.data(), sizeof(ChunkId));
  }
}

bool HalcyonChunkMapper::lookup(ChunkId id, ChunkLocation& out) const {
  std::string value;
  const rocksdb::Status s =
      db_->Get(rocksdb::ReadOptions(), chunkKey(id), &value);
  if (!s.ok()) {
    return false; // includes IsNotFound()
  }
  return deserialize(value, out);
}

void HalcyonChunkMapper::multiLookup(
    const std::vector<ChunkId>& ids,
    std::vector<ChunkLocation>& out,
    std::vector<bool>& found) const {
  out.assign(ids.size(), ChunkLocation{});
  found.assign(ids.size(), false);

  std::vector<rocksdb::Slice> keys;
  keys.reserve(ids.size());
  for (const ChunkId& id : ids) {
    keys.emplace_back(reinterpret_cast<const char*>(&id), sizeof(id));
  }

  std::vector<std::string> values;
  const std::vector<rocksdb::Status> statuses =
      db_->MultiGet(rocksdb::ReadOptions(), keys, &values);
  for (size_t i = 0; i < ids.size(); ++i) {
    if (statuses[i].ok() && deserialize(values[i], out[i])) {
      found[i] = true;
    }
  }
}

void HalcyonChunkMapper::put(ChunkId id, const ChunkLocation& loc) {
  std::string value;
  serialize(loc, value);
  rocksdb::WriteOptions wo;
  wo.sync = syncOnPut_;
  const rocksdb::Status s = db_->Put(wo, chunkKey(id), value);
  if (!s.ok()) {
    throw std::runtime_error(
        fmt::format("chunk map put failed: {}", s.ToString()));
  }
}

void HalcyonChunkMapper::putBatch(
    const std::vector<std::pair<ChunkId, ChunkLocation>>& entries) {
  rocksdb::WriteBatch batch;
  std::string value;
  for (const auto& [id, loc] : entries) {
    value.clear();
    serialize(loc, value);
    batch.Put(chunkKey(id), value);
  }
  rocksdb::WriteOptions wo;
  wo.sync = syncOnPut_;
  const rocksdb::Status s = db_->Write(wo, &batch);
  if (!s.ok()) {
    throw std::runtime_error(
        fmt::format("chunk map putBatch failed: {}", s.ToString()));
  }
}

void HalcyonChunkMapper::syncWal() {
  const rocksdb::Status s = db_->FlushWAL(/*sync=*/true);
  if (!s.ok()) {
    throw std::runtime_error(
        fmt::format("chunk map syncWal failed: {}", s.ToString()));
  }
}

void HalcyonChunkMapper::setMaxChunkId(ChunkId n) {
  maxChunkId_ = n;
  rocksdb::WriteOptions wo;
  wo.sync = syncOnPut_;
  const std::string value(reinterpret_cast<const char*>(&n), sizeof(n));
  const rocksdb::Status s = db_->Put(
      wo, rocksdb::Slice(kMaxChunkIdKey.data(), kMaxChunkIdKey.size()), value);
  if (!s.ok()) {
    throw std::runtime_error(
        fmt::format("chunk map setMaxChunkId failed: {}", s.ToString()));
  }
}

ChunkId HalcyonChunkMapper::randomExistingKey(ChunkId maxId) {
  if (maxId == 0) {
    return 0;
  }
  return folly::Random::rand64(maxId);
}

ChunkId HalcyonChunkMapper::randomLocalKey(
    ChunkId maxId,
    double locality,
    ChunkId window) {
  if (maxId == 0) {
    return 0;
  }
  if (locality > 0.0 && window > 0 && window < maxId &&
      folly::Random::randDouble01() < locality) {
    // Hot window: the most-recently-allocated ids, where reuse concentrates.
    return maxId - window + randomExistingKey(window);
  }
  return randomExistingKey(maxId);
}

} // namespace facebook::halcyon
