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

#include "oldisim/ChildConnection.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdint.h>

#include <memory>
#include <queue>

#include "ChildConnectionImpl.h"
#include "oldisim/Response.h"
#include "oldisim/ResponseContext.h"
#include "oldisim/Util.h"

namespace oldisim {

ChildConnection::ChildConnection(std::unique_ptr<ChildConnectionImpl> impl)
    : impl_(std::move(impl)) {}

ChildConnection::~ChildConnection() {
  bufferevent_free(impl_->bev_);
}

void ChildConnection::Reset() {
  assert(impl_->num_outstanding_requests == 0);
  impl_->read_state_ = ChildConnectionImpl::ReadState::WAITING;
}

void ChildConnection::IssueRequest(uint32_t type, uint64_t request_id,
                                   const void* payload, uint32_t length) {
  // Start tracking the query in the system
  Query query_internal;

  // Set the query header
  query_internal.query_header_.type = type;
  query_internal.query_header_.request_id = request_id;
  query_internal.query_header_.payload_length = length;

  // Start timing begin of operation
  query_internal.query_header_.start_time = GetTimeAccurateNano();

  // Write out operation on the wire
  QueryPacketHeader packet_header =
      std::move(query_internal.GetHeaderNetworkOrder());
  bufferevent_write(impl_->bev_, &packet_header, sizeof(packet_header));
  if (length > 0) {
    bufferevent_write(impl_->bev_, payload, length);
  }

  // Log the request
  impl_->thread_conn_stats_.LogRequest(query_internal);

  // Keep track of one more request_id
  impl_->num_outstanding_requests++;
}

void ChildConnection::set_priority(int pri) {
  if (bufferevent_priority_set(impl_->bev_, pri))
    DIE("bufferevent_set_priority(bev_, %d) failed", pri);
}

int ChildConnection::GetNumOutstandingRequests() const {
  return impl_->num_outstanding_requests;
}

/**
 *  Implementation details for ChildConnectionImpl
 */
ChildConnection::ChildConnectionImpl::ChildConnectionImpl(
    const ResponseCallback& response_handler, const ClosedCallback& _closed_cb,
    event_base* base, const addrinfo* address,
    ChildConnectionStats& thread_conn_stats, bool store_queries, bool no_delay)
    : base_(base),
      closed_cb(_closed_cb),
      response_cb(response_handler),
      no_delay_(no_delay),
      start_time_(GetTimeAccurate()),
      thread_conn_stats_(thread_conn_stats),
      read_state_(ReadState::WAITING),
      num_outstanding_requests(0) {
  // Open socket
  int sockfd = 0;
  if ((sockfd = socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol)) < 0) {
    DIE("\n Error : Could not create socket \n");
  }

  if (connect(sockfd, address->ai_addr, address->ai_addrlen) < 0) {
    char ipstr[INET6_ADDRSTRLEN];
    void* addr;

    // Get the pointer to the address itself,
    // different fields in IPv4 and IPv6:
    if (address->ai_family == AF_INET) {  // IPv4
      struct sockaddr_in* ipv4 = (struct sockaddr_in*)address->ai_addr;
      addr = &(ipv4->sin_addr);
    } else {  // IPv6
      struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)address->ai_addr;
      addr = &(ipv6->sin6_addr);
    }
    inet_ntop(address->ai_family, addr, ipstr, sizeof ipstr);
    DIE("\n Error : Connect Failed to %s \n", ipstr);
  }

  // Make it send back without delay
  if (no_delay_) {
    int optval = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval))) {
      DIE("setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
    }
  }

  // Make it non-blocking
  evutil_make_socket_nonblocking(sockfd);

  // Make buffer event
  bev_ = bufferevent_socket_new(base_, sockfd, BEV_OPT_CLOSE_ON_FREE);
}

// The followings are C trampolines for libevent callbacks.
void ChildConnection::ChildConnectionImpl::bev_event_cb(struct bufferevent* bev,
                                                        int16_t events,
                                                        void* ptr) {
  ChildConnection* conn = reinterpret_cast<ChildConnection*>(ptr);

  if (events & BEV_EVENT_TIMEOUT) {
    DIE("Timeout from child");
  } else if (events & BEV_EVENT_ERROR) {
  } else if (events & BEV_EVENT_EOF) {
    D("Child closed connection");
    conn->impl_->read_state_ = ReadState::CLOSED;
    bufferevent_disable(conn->impl_->bev_, EV_READ | EV_WRITE);
    if (conn->impl_->closed_cb != nullptr) {
      conn->impl_->closed_cb(*conn);
    }
  }
}

/**
 * Check to see if the buffer contains at least one full response that is ready
 * for retrieval. If so, it returns true; false otherwise
 *
 * @param input evbuffer to read response from
 * @return true if a response is ready to be read, false if not enough data in
 *buffer
 */
static bool BufferContainsResponse(evbuffer* input) {
  // Check length of input buffer
  size_t buffer_length = evbuffer_get_length(input);
  const size_t header_length = sizeof(ResponsePacketHeader);
  if (buffer_length < header_length) {
    return false;
  }
  ResponsePacketHeader* h = reinterpret_cast<ResponsePacketHeader*>(
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

void ChildConnection::ChildConnectionImpl::bev_read_cb(struct bufferevent* bev,
                                                       void* ptr) {
  ChildConnection* conn = reinterpret_cast<ChildConnection*>(ptr);
  struct evbuffer* input = bufferevent_get_input(conn->impl_->bev_);

  // Protocol processing loop.
  if (conn->impl_->num_outstanding_requests == 0) {
    V("Spurious read callback.");
    return;
  }

  while (true) {
    switch (conn->impl_->read_state_) {
      case ReadState::CLOSED: {
        DIE("event from closed connection");
      }
      case ReadState::WAITING: {
        if (BufferContainsResponse(input)) {
          // Read out response header
          const size_t header_length = sizeof(ResponsePacketHeader);

          ResponsePacketHeader* h = reinterpret_cast<ResponsePacketHeader*>(
              evbuffer_pullup(input, header_length));
          assert(h);
          Response response = Response::FromHeaderNetworkOrder(h);

          // Remove the header
          evbuffer_drain(input, header_length);

          // Linearize evbuffer to allow for payload reading
          void* payload =
              evbuffer_pullup(input, response.response_header_.payload_length);
          response.payload_ = payload;

          uint64_t request_id = response.GetRequestID();

          // Log query statistics
          Query originating_query = response.RebuildOriginatingQuery();
          originating_query.end_time_ = GetTimeAccurateNano();

          // Drained one request
          conn->impl_->num_outstanding_requests--;

          // Call user provided callback, handing off full packet
          ResponseContext context(
              response.GetType(), response.GetRequestID(),
              response.GetPayloadLength(), response.GetResponsePacketSize(),
              response.payload_, false, originating_query.GetStartTime(),
              originating_query.end_time_);
          assert(conn->impl_->response_cb != nullptr);
          conn->impl_->response_cb(context);

          // Remove the payload from the evbuffer
          evbuffer_drain(input, response.response_header_.payload_length);

          conn->impl_->thread_conn_stats_.LogResponse(originating_query,
                                                      response);
          break;
        } else {
          return;
        }
      }
      default: { DIE("not implemented"); }
    }
  }
}

void ChildConnection::ChildConnectionImpl::bev_write_cb(struct bufferevent* bev,
                                                        void* ptr) {
  ChildConnection* conn = reinterpret_cast<ChildConnection*>(ptr);
  // Currently write cb does nothing
}
}  // namespace oldisim

