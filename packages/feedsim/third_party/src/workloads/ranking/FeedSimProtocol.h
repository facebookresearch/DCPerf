// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <endian.h>
#include <stdint.h>
#include <time.h>

namespace feedsim {

// Wire protocol structs — binary compatible with oldisim's Query.h / Response.h
// All fields are big-endian on the wire.

struct __attribute__((__packed__)) QueryPacketHeader {
  uint32_t type;
  uint64_t request_id;
  uint64_t start_time;
  uint32_t payload_length; // does not include header length
};

struct __attribute__((__packed__)) ResponsePacketHeader {
  uint32_t type;
  uint64_t request_id;
  uint64_t start_time;
  uint64_t processing_time;
  uint32_t payload_length; // does not include header length
};

static_assert(sizeof(QueryPacketHeader) == 24, "QueryPacketHeader must be 24 bytes");
static_assert(sizeof(ResponsePacketHeader) == 32, "ResponsePacketHeader must be 32 bytes");

inline QueryPacketHeader queryToNetwork(const QueryPacketHeader& h) {
  QueryPacketHeader out;
  out.type = htobe32(h.type);
  out.request_id = htobe64(h.request_id);
  out.start_time = htobe64(h.start_time);
  out.payload_length = htobe32(h.payload_length);
  return out;
}

inline QueryPacketHeader queryFromNetwork(const QueryPacketHeader& h) {
  QueryPacketHeader out;
  out.type = be32toh(h.type);
  out.request_id = be64toh(h.request_id);
  out.start_time = be64toh(h.start_time);
  out.payload_length = be32toh(h.payload_length);
  return out;
}

inline ResponsePacketHeader responseToNetwork(const ResponsePacketHeader& h) {
  ResponsePacketHeader out;
  out.type = htobe32(h.type);
  out.request_id = htobe64(h.request_id);
  out.start_time = htobe64(h.start_time);
  out.processing_time = htobe64(h.processing_time);
  out.payload_length = htobe32(h.payload_length);
  return out;
}

inline ResponsePacketHeader responseFromNetwork(const ResponsePacketHeader& h) {
  ResponsePacketHeader out;
  out.type = be32toh(h.type);
  out.request_id = be64toh(h.request_id);
  out.start_time = be64toh(h.start_time);
  out.processing_time = be64toh(h.processing_time);
  out.payload_length = be32toh(h.payload_length);
  return out;
}

inline uint64_t getTimeNano() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

inline double getTimeSec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return ts.tv_sec + static_cast<double>(ts.tv_nsec) / 1e9;
}

} // namespace feedsim
