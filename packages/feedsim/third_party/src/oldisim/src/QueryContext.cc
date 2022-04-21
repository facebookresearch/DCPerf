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

#include "oldisim/QueryContext.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

#include "oldisim/Log.h"
#include "oldisim/Util.h"

namespace oldisim {

QueryContext::QueryContext(ParentConnection& _connection, uint32_t _type,
                           uint64_t _request_id, uint64_t _start_time,
                           uint32_t _payload_length, uint32_t _packet_length,
                           void* _payload, bool _is_payload_heap)
    : connection(_connection),
      type(_type),
      request_id(_request_id),
      start_time(_start_time),
      received_time(GetTimeAccurateNano()),
      payload_length(_payload_length),
      packet_length(_packet_length),
      payload(_payload),
      response_sent(false),
      is_active(true),
      is_payload_heap(_is_payload_heap),
      logger(nullptr) {}

QueryContext::QueryContext(QueryContext&& other)
    : connection(other.connection),
      type(other.type),
      request_id(other.request_id),
      start_time(other.start_time),
      received_time(other.received_time),
      payload_length(other.payload_length),
      packet_length(other.packet_length),
      payload(other.is_payload_heap ? other.payload : malloc(payload_length)),
      response_sent(other.response_sent),
      is_active(other.is_active),
      logger(other.logger) {
  // Make copy of payload to newly malloced memory if other context was
  // not allocated in the heap
  if (!other.is_payload_heap) {
    memcpy(payload, other.payload, payload_length);
  }
  is_payload_heap = true;

  // Clear out other object
  other.response_sent = false;
  other.is_active = false;
  other.is_payload_heap = false;
  other.logger = nullptr;
}

QueryContext::~QueryContext() {
  if (is_active && !response_sent) {
    W("Query of type %d and length %d was not responded to", type,
      payload_length);
    assert(false);
  }
  // Cleanup if this is a moved copy
  if (is_payload_heap) {
    free(payload);
  }
}

void QueryContext::SendResponse(const void* data, uint32_t data_length) {
  // Make sure this is first time sending a response
  assert(!response_sent);

  // Send it over the wire
  uint64_t processing_time = GetTimeAccurateNano() - received_time;
  connection.SendResponse(type, request_id, start_time, processing_time, data,
                          data_length, logger);
  response_sent = true;
}
}  // namespace oldisim
