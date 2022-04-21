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

#ifndef CHILD_CONNECTION_IMPL_H
#define CHILD_CONNECTION_IMPL_H

#include <netdb.h>
#include <netinet/tcp.h>
#include <string.h>
#include <stdint.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <string>
#include <unordered_map>

#include "oldisim/ChildConnection.h"
#include "oldisim/ChildConnectionStats.h"
#include "InternalCallbacks.h"

namespace oldisim {

class Query;

class ChildConnection::ChildConnectionImpl {
 public:
  event_base *base_;
  bufferevent *bev_;
  bool no_delay_;

  double start_time_;  // Time when this connection began operations.

  ChildConnectionStats &
      thread_conn_stats_;  // Aggregates data over whole thread

  enum class ReadState {
    WAITING,
    CLOSED,
  };
  ReadState read_state_;

  uint32_t num_outstanding_requests;

  typedef std::function<void(const ChildConnection &conn)> ClosedCallback;
  ClosedCallback closed_cb;

  ResponseCallback response_cb;

  ChildConnectionImpl(const ResponseCallback &response_handler,
                      const ClosedCallback &_closed_cb, event_base *base,
                      const addrinfo *address,
                      ChildConnectionStats &thread_conn_stats,
                      bool store_queries, bool no_delay);

  // The followings are C trampolines for libevent callbacks.
  static void bev_event_cb(struct bufferevent *bev, int16_t events, void *ptr);
  static void bev_read_cb(struct bufferevent *bev, void *ptr);
  static void bev_write_cb(struct bufferevent *bev, void *ptr);
};
}  // namespace oldisim

#endif  // CHILD_CONNECTION_IMPL_H

