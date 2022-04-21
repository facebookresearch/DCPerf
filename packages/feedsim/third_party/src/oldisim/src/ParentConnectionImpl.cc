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

#include "ParentConnectionImpl.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <string.h>

#include "ConnectionUtil.h"
#include "oldisim/Callbacks.h"
#include "oldisim/Query.h"
#include "oldisim/QueryContext.h"

namespace oldisim {

ParentConnection::ParentConnectionImpl::ParentConnectionImpl(
    const ParentConnectionReceivedCallback& _request_handler,
    const ClosedCallback& _closed_cb, bufferevent* _bev, bool _use_locking)
    : request_handler(_request_handler),
      bev(_bev),
      read_state(ReadState::INIT_READ),
      closed_cb(_closed_cb),
      use_locking(_use_locking) {}

ParentConnection::ParentConnectionImpl::~ParentConnectionImpl() {
  bufferevent_disable(bev, EV_READ | EV_WRITE);
  bufferevent_free(bev);
}

// The followings are C trampolines for libevent callbacks
void ParentConnection::ParentConnectionImpl::bev_event_cb(bufferevent* bev,
                                                          int16_t events,
                                                          void* ptr) {
  ParentConnection* conn = reinterpret_cast<ParentConnection*>(ptr);
  if (events & BEV_EVENT_TIMEOUT) {
    DIE("Timeout from leaf");
  } else if (events & BEV_EVENT_ERROR) {
  } else if (events & BEV_EVENT_EOF) {
    D("Parent closed connection");
    conn->impl_->read_state = ReadState::CLOSED;
    bufferevent_disable(conn->impl_->bev, EV_READ | EV_WRITE);
    if (conn->impl_->closed_cb != nullptr) {
      conn->impl_->closed_cb(*conn);
    }
  }
}

/**
 * Check to see if the buffer contains at least one full query that is ready
 * for retrieval. If so, it returns true; false otherwise
 *
 * @param input evbuffer to read query from
 * @return true if a query is ready to be read, false if not enough data in
 *buffer
 */
static bool BufferContainsQuery(evbuffer* input) {
  // Check length of input buffer
  size_t buffer_length = evbuffer_get_length(input);
  const size_t header_length = sizeof(QueryPacketHeader);
  if (buffer_length < header_length) {
    return false;
  }
  QueryPacketHeader* h = reinterpret_cast<QueryPacketHeader*>(
      evbuffer_pullup(input, header_length));
  assert(h);

  // Not whole query
  uint32_t packet_length = header_length + be32toh(h->payload_length);
  if (buffer_length < packet_length) {
    return false;
  }

  // Must be full query
  return true;
}

void ParentConnection::ParentConnectionImpl::bev_read_cb(bufferevent* bev,
                                                         void* ptr) {
  ParentConnection* conn = reinterpret_cast<ParentConnection*>(ptr);
  evbuffer* input = bufferevent_get_input(bev);
  int num_queries_processed = 0;

  while (true) {
    switch (conn->impl_->read_state) {
      case ReadState::INIT_READ: {
        DIE("event from uninitialized connection");
      }
      case ReadState::CLOSED: {
        DIE("event from closed connection");
      }
      case ReadState::WAITING: {
        if (BufferContainsQuery(input)) {
          // Read out entire query
          const size_t header_length = sizeof(QueryPacketHeader);
          QueryPacketHeader* h = reinterpret_cast<QueryPacketHeader*>(
              evbuffer_pullup(input, header_length));
          assert(h);

          // Get the header information
          uint32_t type = be32toh(h->type);
          uint64_t query_id = be64toh(h->request_id);
          uint64_t start_time = be64toh(h->start_time);
          uint32_t payload_length = be32toh(h->payload_length);
          uint32_t packet_length = header_length + payload_length;

          // Remove the header
          evbuffer_drain(input, header_length);

          // Linearize evbuffer to allow for payload reading
          void* payload = evbuffer_pullup(input, payload_length);

          // Create query context
          QueryContext context(*conn, type, query_id, start_time,
                               payload_length, packet_length, payload, false);

          // Call callback, handing off full query context and # of query
          // processed in this loop
          conn->impl_->request_handler(context, num_queries_processed++);

          // Remove the data from evbuffer
          evbuffer_drain(input, payload_length);

          break;  // leaves switch statement, not while loop
        } else {
          return;
        }
      }
      default: { DIE("not implemented"); }
    }
  }
}

void ParentConnection::ParentConnectionImpl::bev_write_cb(bufferevent* bev,
                                                          void* ptr) {
  ParentConnection* conn = reinterpret_cast<ParentConnection*>(ptr);
}
}  // namespace oldisim
