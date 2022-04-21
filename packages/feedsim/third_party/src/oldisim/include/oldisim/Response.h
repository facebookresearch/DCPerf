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

#ifndef OLDISIM_RESPONSE_H
#define OLDISIM_RESPONSE_H

#include <stdint.h>

#include <memory>
#include <string>

#include "oldisim/Query.h"

namespace oldisim {

struct __attribute__((__packed__)) ResponsePacketHeader {
  uint32_t type;
  uint64_t request_id;
  uint64_t start_time;
  uint64_t processing_time;
  uint32_t payload_length;  // does not include header length

  ResponsePacketHeader(uint32_t _type, uint64_t _request_id,
                       uint64_t _start_time, uint64_t _processing_time,
                       uint32_t _payload_length)
      : type(_type),
        request_id(_request_id),
        start_time(_start_time),
        processing_time(_processing_time),
        payload_length(_payload_length) {}
};

/**
 * Internal tracking data structure of a response going back up the tree.
 * The user does not directly interact with this, only with blobs
 * of data
 */
class Response {
 public:
  ResponsePacketHeader response_header_;
  const void* payload_;  // only used to store received response

  Response() : response_header_(0, 0, 0, 0, 0), payload_(nullptr) {}

  Response(uint32_t type, uint64_t request_id, uint64_t start_time,
           uint64_t processing_time, uint32_t payload_length)
      : response_header_(type, request_id, start_time, processing_time,
                         payload_length),
        payload_(nullptr) {}

  ResponsePacketHeader GetHeaderNetworkOrder() const {
    ResponsePacketHeader header = response_header_;
    header.type = htobe32(header.type);
    header.request_id = htobe64(header.request_id);
    header.start_time = htobe64(header.start_time);
    header.processing_time = htobe64(header.processing_time);
    header.payload_length = htobe32(header.payload_length);

    return header;
  }

  static Response FromHeaderNetworkOrder(const ResponsePacketHeader* header) {
    Response result;
    ResponsePacketHeader& result_header = result.response_header_;
    result_header.type = be32toh(header->type);
    result_header.request_id = be64toh(header->request_id);
    result_header.start_time = be64toh(header->start_time);
    result_header.processing_time = be64toh(header->processing_time);
    result_header.payload_length = be32toh(header->payload_length);

    return result;
  }

  uint32_t GetResponsePacketSize() const {
    return response_header_.payload_length + sizeof(response_header_);
  }

  uint32_t GetPayloadLength() const { return response_header_.payload_length; }

  uint32_t GetType() const { return response_header_.type; }

  uint64_t GetRequestID() const { return response_header_.request_id; }

  uint64_t GetStartTime() const { return response_header_.start_time; }

  uint64_t GetProcessingTime() const {
    return response_header_.processing_time;
  }

  Query RebuildOriginatingQuery() const {
    Query originating_query;
    originating_query.query_header_.type = response_header_.type;
    originating_query.query_header_.request_id = response_header_.request_id;
    originating_query.query_header_.start_time = response_header_.start_time;

    return originating_query;
  }
};
}  // namespace oldisim

#endif  // OLDISIM_RESPONSE_H

