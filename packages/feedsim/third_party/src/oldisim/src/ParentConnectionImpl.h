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

// -*- c++-mode -*-

#ifndef PARENT_CONNECTION_IMPL_H
#define PARENT_CONNECTION_IMPL_H

#include <inttypes.h>
#include <event2/bufferevent.h>

#include <functional>
#include <set>
#include <mutex>
#include <unordered_map>

#include "oldisim/Callbacks.h"
#include "InternalCallbacks.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"

namespace oldisim {

struct ParentConnection::ParentConnectionImpl {
  const ParentConnectionReceivedCallback request_handler;

  bufferevent* const bev;

  typedef std::function<void(const ParentConnection& conn)> ClosedCallback;
  ClosedCallback closed_cb;

  enum class ReadState {
    INIT_READ,
    WAITING,
    CLOSED,
  };
  ReadState read_state;

  std::mutex sending_lock;
  bool use_locking;

  ParentConnectionImpl(const ParentConnectionReceivedCallback& _request_handler,
                       const ClosedCallback& _closed_cb, bufferevent* _bev,
                       bool _use_locking);
  ~ParentConnectionImpl();

  static void bev_event_cb(struct bufferevent* bev, int16_t events, void* ptr);
  static void bev_read_cb(struct bufferevent* bev, void* ptr);
  static void bev_write_cb(struct bufferevent* bev, void* ptr);
};
}  // namespace oldisim

#endif  // PARENT_CONNECTION_IMPL_H
