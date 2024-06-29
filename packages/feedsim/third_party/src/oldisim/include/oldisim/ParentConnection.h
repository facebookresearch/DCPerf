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

#ifndef OLDISIM_PARENT_CONNECTION_H
#define OLDISIM_PARENT_CONNECTION_H

#include <stdint.h>
#include <event2/event.h>

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>

namespace oldisim {

class ConnectionUtil;
class QueryContext;
class ParentConnectionStats;
class Response;

/**
 * This classes represents one part of a bi-directional connection between
 * two nodes in the fanout tree. Specifically, this class is owned by the
 * child node in the tree and represents the connection established to the child
 * node by the parent node of the tree. This class allows the child to
 * send replies to requests sent to the child by the parent node.
 */
class ParentConnection {
  friend QueryContext;
  friend ConnectionUtil;

 public:
  ~ParentConnection();
  ParentConnection(const ParentConnection& that) = delete;
  void SendResponse(uint32_t response_type, uint64_t query_id,
                    uint64_t start_time, uint64_t processing_time,
                    const void* data, uint32_t data_length,
                    std::function<void(const Response&)> logger = nullptr);

 private:
  struct ParentConnectionImpl;
  const std::unique_ptr<ParentConnectionImpl> impl_;

  explicit ParentConnection(std::unique_ptr<ParentConnectionImpl> impl);
};
}  // namespace oldisim

#endif  // OLDISIM_PARENT_CONNECTION_H

