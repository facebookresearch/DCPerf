// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>

namespace oldisim {

struct __attribute__((__packed__)) QueryPacketHeader {
  uint32_t type;
  uint64_t request_id;
  uint64_t start_time;
  uint32_t payload_length;  // does not include header length
};

/**
 * Internal tracking data structure of a query.
 * The user does not directly interact with this, only with blobs
 * of data
 */
class Query {
 public:
  uint64_t end_time_;
  QueryPacketHeader query_header_;

  uint64_t Time() const { return (end_time_ - query_header_.start_time); }

  uint32_t GetQueryPacketSize() const {
    return query_header_.payload_length + sizeof(query_header_);
  }

  uint32_t GetPayloadLength() const { return query_header_.payload_length; }

  uint32_t GetType() const { return query_header_.type; }

  uint64_t GetRequestID() const { return query_header_.request_id; }

  uint64_t GetStartTime() const { return query_header_.start_time; }

  QueryPacketHeader GetHeaderNetworkOrder() const {
    QueryPacketHeader header = query_header_;
    header.type = htobe32(header.type);
    header.request_id = htobe64(header.request_id);
    header.start_time = htobe64(header.start_time);
    header.payload_length = htobe32(header.payload_length);

    return header;
  }
};
}  // namespace oldisim
