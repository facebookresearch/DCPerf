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

#ifndef OLDISIM_QUERY_CONTEXT_H
#define OLDISIM_QUERY_CONTEXT_H

#include <functional>
#include <memory>

#include "oldisim/ParentConnection.h"

namespace oldisim {

class ParentConnection;
class LeafNodeServer;
class ParentNodeServer;

class QueryContext {
  friend ParentConnection;
  friend LeafNodeServer;
  friend ParentNodeServer;

 public:
  QueryContext(QueryContext&& other);
  QueryContext(const QueryContext& other) = delete;
  ~QueryContext();

  const uint32_t type;
  const uint64_t request_id;
  const uint64_t start_time;
  const uint64_t received_time;
  const uint32_t payload_length;
  const uint32_t packet_length;
  void* const payload;
  void SendResponse(const void* data, uint32_t data_length);

 private:
  ParentConnection& connection;
  bool response_sent;
  bool is_active;
  bool is_payload_heap;
  std::function<void(const Response&)> logger;

  QueryContext(ParentConnection& _connection, uint32_t _type,
               uint64_t _request_id, uint64_t start_time,
               uint32_t _payload_length, uint32_t _packet_length,
               void* _payload, bool _is_payload_heap);
};
}  // namespace oldisim

#endif  // OLDISIM_QUERY_CONTEXT_H

