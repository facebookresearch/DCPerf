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

#include "oldisim/ParentConnection.h"

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <string.h>

#include "ParentConnectionImpl.h"
#include "oldisim/LeafNodeServer.h"
#include "oldisim/Response.h"

namespace oldisim {

ParentConnection::ParentConnection(std::unique_ptr<ParentConnectionImpl> impl)
    : impl_(move(impl)) {}

ParentConnection::~ParentConnection() {}

void ParentConnection::SendResponse(
    uint32_t response_type, uint64_t query_id, uint64_t start_time,
    uint64_t processing_time, const void* data, uint32_t data_length,
    std::function<void(const Response&)> logger) {
  Response response(response_type, query_id, start_time, processing_time,
                    data_length);

  // Send it over the wire
  {
    std::unique_lock<std::mutex> lock;
    // Grab the lock if locking is enabled to avoid split responses
    if (impl_->use_locking) {
      lock = std::unique_lock<std::mutex>(impl_->sending_lock);
    }
    ResponsePacketHeader header = std::move(response.GetHeaderNetworkOrder());
    bufferevent_write(impl_->bev, &header, sizeof(header));
    if (data_length > 0) {
      bufferevent_write(impl_->bev, data, data_length);
    }
  }

  // Update stats
  if (logger != nullptr) {
    logger(response);
  }
}
}  // namespace oldisim
