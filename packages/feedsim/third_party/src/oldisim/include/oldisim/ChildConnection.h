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

#ifndef OLDISIM_CHILD_CONNECTION_H
#define OLDISIM_CHILD_CONNECTION_H

#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/event.h>
#include <event2/util.h>

#include <set>
#include <string>

#include "oldisim/ChildConnectionStats.h"

namespace oldisim {

class ConnectionUtil;
class Query;

/**
 * This classes represents one part of a bi-directional connection between
 * two nodes in the fanout tree. Specifically, this class is owned by the
 * parent node in the tree and represents the connection established to the
 * child node by the parent node of the tree. This class allows the parent to
 * send requests to the child node.
 */
class ChildConnection {
  friend ConnectionUtil;

 public:
  ~ChildConnection();

  void IssueRequest(uint32_t type, uint64_t request_id, const void* payload,
                    uint32_t length);
  void Reset();

  void set_priority(int pri);
  int GetNumOutstandingRequests() const;

 private:
  class ChildConnectionImpl;
  const std::unique_ptr<ChildConnectionImpl> impl_;
  explicit ChildConnection(std::unique_ptr<ChildConnectionImpl> impl);
};
}  // namespace oldisim

#endif  // OLDISIM_CHILD_CONNECTION_H

