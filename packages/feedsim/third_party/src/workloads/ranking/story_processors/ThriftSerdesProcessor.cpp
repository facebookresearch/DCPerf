// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "ThriftSerdesProcessor.h"

#include <cstring>

namespace dcperf {
namespace story_processors {

namespace {
// Thread-local scratch buffer reused across pipeline passes — avoids
// per-call allocation in the hot path. Sized at 16 KB to comfortably
// hold a worst-case 100-story batch of serialized score-infos.
thread_local SerdesBuffer t_serdes_buffer;

// Match prod's thrift zigzag encoding (apache::thrift::CompactProtocol
// internally calls signedIntToZigzag for every signed field). This is
// the leaf at ~0.45% of prod story CPU.
inline uint64_t signedIntToZigzag64(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}
inline uint32_t signedIntToZigzag32(int32_t v) {
  return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

// Varint write — 1-9 bytes for u64, 1-5 bytes for u32. Matches
// apache::thrift::util::writeVarintUnrolled instruction mix.
inline size_t writeVarint64(uint8_t* out, uint64_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    out[n++] = static_cast<uint8_t>(v) | 0x80u;
    v >>= 7;
  }
  out[n++] = static_cast<uint8_t>(v);
  return n;
}

inline void writeFieldI64(SerdesBuffer& buf, int64_t v) {
  uint8_t tmp[10];
  size_t n = writeVarint64(tmp, signedIntToZigzag64(v));
  buf.insert(buf.end(), tmp, tmp + n);
}

inline void writeFieldI32(SerdesBuffer& buf, int32_t v) {
  uint8_t tmp[5];
  size_t n = writeVarint64(tmp, signedIntToZigzag32(v));
  buf.insert(buf.end(), tmp, tmp + n);
}

inline void writeFieldFloat(SerdesBuffer& buf, float v) {
  uint8_t tmp[4];
  std::memcpy(tmp, &v, 4);
  buf.insert(buf.end(), tmp, tmp + 4);
}
} // namespace

void ThriftSerdesProcessor::initialize(int complexity, uint64_t seed) {
  complexity_ = complexity;
  (void)seed;
}

void ThriftSerdesProcessor::process(MockStoryList& stories, MockQueryCtx& ctx) {
  // Reset (not free) the thread-local buffer. capacity persists across
  // requests so we never re-alloc.
  t_serdes_buffer.clear();
  // Reserve ~128 B per story to match prod's ~5% serialization-bucket
  // size on story workload (60-80 B typical, with header overhead).
  t_serdes_buffer.reserve(stories.size() * 128);

  for (const auto& s : stories) {
    // Emit 12 fields per story-score-info, mirroring prod's
    // ViewStateStoryInfo wire layout.
    writeFieldI64(t_serdes_buffer, s.score_info.story_key);
    writeFieldI64(t_serdes_buffer, s.score_info.actor_id);
    writeFieldI64(t_serdes_buffer, s.score_info.target_id);
    writeFieldI32(t_serdes_buffer, s.score_info.source_type);
    writeFieldI32(t_serdes_buffer, s.score_info.story_type);
    writeFieldI32(t_serdes_buffer, s.score_info.time_published);
    writeFieldFloat(t_serdes_buffer, s.score_info.weight);
    writeFieldFloat(t_serdes_buffer, s.score_info.weight_user);
    writeFieldFloat(t_serdes_buffer, s.score_info.weight_event);
    writeFieldFloat(t_serdes_buffer, s.score_info.discounted_weight);
    writeFieldI32(t_serdes_buffer, static_cast<int32_t>(s.score_info.flags));

    // Length-prefixed payload (the variable-length blob field). Use the
    // story's raw_scores backing as the source — gives a realistic
    // memcpy of 32 B at default (8 entries × 4 B). Matches prod's
    // story_payload binary field in RankedStoryInfo.
    const auto payload_bytes = s.raw_scores.size() * sizeof(float);
    writeFieldI32(t_serdes_buffer, static_cast<int32_t>(payload_bytes));
    if (payload_bytes > 0) {
      const auto pos = t_serdes_buffer.size();
      t_serdes_buffer.resize(pos + payload_bytes);
      std::memcpy(
          t_serdes_buffer.data() + pos,
          s.raw_scores.data(),
          payload_bytes);
    }
  }
  (void)ctx;
}

} // namespace story_processors
} // namespace dcperf
