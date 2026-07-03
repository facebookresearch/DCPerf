// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Server-side response generators that source their bytes from the Silesia
// corpus instead of xor128() RNG. The original RankingGenerators.h burns
// ~15% of leaf CPU on xor128 calls (mersenne_twister, RNG-derived hashes,
// generateRandomString, etc.) — work that has no analog in production. By
// reading bytes from a mmap'd Silesia file we keep field values varied
// per-request without paying for RNG, and the bytes have realistic entropy
// for downstream ZSTD compression.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#include <ranking/SilesiaLoader.h>
#include <ranking/if/gen-cpp2/ranking_types.h>

namespace ranking {
namespace generators {

// Cursor that walks a Silesia file linearly, wrapping at the end. A single
// cursor is meant to be used by one thread (no internal locking).
class SilesiaCursor {
 public:
  SilesiaCursor() = default;
  SilesiaCursor(const uint8_t* data, size_t size, size_t offset = 0)
      : data_(data), size_(size), pos_(size > 0 ? offset % size : 0) {}

  template <typename T>
  T next() {
    static_assert(std::is_trivially_copyable_v<T>);
    if (data_ == nullptr || size_ < sizeof(T)) {
      return T{};
    }
    if (pos_ + sizeof(T) > size_) {
      pos_ = 0;
    }
    T v;
    std::memcpy(&v, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  // Take `n` bytes as a string. Wraps to the start if needed.
  std::string takeString(size_t n) {
    if (data_ == nullptr || size_ == 0 || n == 0) {
      return std::string();
    }
    std::string s;
    s.resize(n);
    size_t copied = 0;
    while (copied < n) {
      if (pos_ >= size_) pos_ = 0;
      size_t chunk = std::min(n - copied, size_ - pos_);
      std::memcpy(s.data() + copied, data_ + pos_, chunk);
      pos_ += chunk;
      copied += chunk;
    }
    return s;
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

// Generates RankingResponse / RankingObject / RankingStory objects whose
// field values come from Silesia bytes rather than xor128. Thread-safe: each
// call atomically claims a starting offset in the corpus so concurrent calls
// do not produce identical output.
class SilesiaResponseGenerator {
 public:
  // `loader` must outlive this object. May be nullptr — in that case the
  // generator falls back to producing all-zero values (cheap, no RNG).
  explicit SilesiaResponseGenerator(const SilesiaLoader* loader)
      : loader_(loader), bumper_(0) {}

  ranking::RankingObject generateRankingObject(size_t actions_length) {
    SilesiaCursor cur = makeCursor();
    return generateRankingObject(cur, actions_length);
  }

  ranking::RankingStory generateRankingStory(size_t ranking_objects_length) {
    SilesiaCursor cur = makeCursor();
    return generateRankingStory(cur, ranking_objects_length);
  }

  // Top-level entry: drop-in replacement for
  // generators::generateRandomRankingResponse(N).
  ranking::RankingResponse generateRankingResponse(
      size_t ranking_stories_length) {
    SilesiaCursor cur = makeCursor();
    ranking::RankingResponse resp;
    resp.queryID() = cur.next<int64_t>();
    auto& stories = *resp.rankingStories();
    stories.reserve(ranking_stories_length);
    for (size_t i = 0; i < ranking_stories_length; ++i) {
      stories.push_back(generateRankingStory(cur, /*ranking_objects_length=*/20));
    }
    auto& counts = *resp.objectCounts();
    counts.reserve(ranking_stories_length);
    for (size_t i = 0; i < ranking_stories_length; ++i) {
      counts.push_back(cur.next<int32_t>());
    }
    resp.metadata() = cur.takeString(200);
    return resp;
  }

 private:
  // Atomically claim a starting offset so concurrent threads diverge.
  SilesiaCursor makeCursor() {
    if (loader_ == nullptr || !loader_->isLoaded() ||
        loader_->numFiles() == 0) {
      return SilesiaCursor();
    }
    // Pick a file based on a bumper counter so different concurrent calls
    // get different files (load-balances across the corpus).
    size_t idx = bumper_.fetch_add(1, std::memory_order_relaxed);
    const auto& f = loader_->fileAt(idx % loader_->numFiles());
    // Distribute starting offsets across the file too.
    size_t offset = idx * 4096;
    return SilesiaCursor(f.data, f.size, offset);
  }

  ranking::Action generateAction(SilesiaCursor& cur) {
    ranking::Action a;
    a.type() = cur.next<int16_t>();
    a.timeUsec() = cur.next<int64_t>();
    a.timeMsec() = cur.next<int32_t>();
    a.actorID() = cur.next<int64_t>();
    return a;
  }

  ranking::RankingPayloadIntMap generateIntMap(
      SilesiaCursor& cur, size_t length) {
    ranking::RankingPayloadIntMap m;
    m.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      m.emplace(cur.next<int16_t>(), cur.next<int64_t>());
    }
    return m;
  }

  ranking::RankingPayloadStringMap generateStringMap(
      SilesiaCursor& cur, size_t length) {
    ranking::RankingPayloadStringMap m;
    m.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      m.emplace(cur.next<int16_t>(), cur.takeString(25));
    }
    return m;
  }

  ranking::RankingPayloadVecMap generateVecMap(
      SilesiaCursor& cur, size_t length) {
    ranking::RankingPayloadVecMap m;
    m.reserve(length);
    for (size_t i = 0; i < length; ++i) {
      ranking::SmallListI64 v;
      v.reserve(10);
      for (int j = 0; j < 10; ++j) {
        v.push_back(cur.next<int64_t>());
      }
      m.emplace(cur.next<int16_t>(), std::move(v));
    }
    return m;
  }

  ranking::RankingObject generateRankingObject(
      SilesiaCursor& cur, size_t actions_length) {
    ranking::RankingObject obj;
    obj.objectID() = cur.next<int64_t>();
    obj.objectType() = static_cast<ranking::RankingObjectType>(
        static_cast<uint64_t>(cur.next<int64_t>()) %
        static_cast<uint64_t>(ranking::RankingObjectType::OBJ_TYPE_Z));
    obj.actorID() = cur.next<int64_t>();
    obj.createTime() = cur.next<int64_t>();
    obj.payloadIntMap() = generateIntMap(cur, 5);
    obj.payloadStrMap() = generateStringMap(cur, 5);
    obj.payloadVecMap() = generateVecMap(cur, 5);
    auto& actions = *obj.actions();
    actions.reserve(actions_length);
    for (size_t i = 0; i < actions_length; ++i) {
      actions.push_back(generateAction(cur));
    }
    obj.weight() = static_cast<double>(cur.next<int64_t>());
    return obj;
  }

  ranking::RankingStory generateRankingStory(
      SilesiaCursor& cur, size_t ranking_objects_length) {
    ranking::RankingStory story;
    story.storyID() = cur.next<int64_t>();
    auto& objects = *story.objects();
    objects.reserve(ranking_objects_length);
    for (size_t i = 0; i < ranking_objects_length; ++i) {
      objects.push_back(generateRankingObject(cur, /*actions_length=*/5));
    }
    story.weight() = static_cast<double>(cur.next<int64_t>());
    story.storyType() = static_cast<ranking::RankingStoryType>(
        static_cast<uint64_t>(cur.next<int64_t>()) %
        static_cast<uint64_t>(ranking::RankingStoryType::STORY_TYPE_Z));
    return story;
  }

  const SilesiaLoader* loader_;
  std::atomic<size_t> bumper_;
};

} // namespace generators
} // namespace ranking
