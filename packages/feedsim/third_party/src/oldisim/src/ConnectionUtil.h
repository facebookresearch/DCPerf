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

#ifndef CONNECTION_UTIL_H
#define CONNECTION_UTIL_H

#include <inttypes.h>
#include <netdb.h>
#include <event2/event.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "oldisim/Callbacks.h"
#include "oldisim/ChildConnection.h"
#include "ChildConnectionImpl.h"
#include "oldisim/ParentConnection.h"
#include "ParentConnectionImpl.h"

namespace oldisim {

class ChildConnectionStats;
class LeafNodeStats;

class ConnectionUtil {
 public:
  template <template <typename, typename, typename, typename, typename>
            class MapContainer,
            typename Handler, typename A, typename B, typename C>
  static std::set<uint32_t> GetQueryTypes(
      const MapContainer<uint32_t, Handler, A, B, C>& handlers) {
    std::set<uint32_t> types;
    for (auto& h : handlers) {
      types.insert(h.first);
    }

    return types;
  }

  static std::unique_ptr<ChildConnection> MakeChildConnection(
      const ResponseCallback& response_handler,
      const ChildConnection::ChildConnectionImpl::ClosedCallback& close_handler,
      const NodeThread& node_thread, const addrinfo* address,
      ChildConnectionStats& thread_conn_stats, bool store_queries,
      bool no_delay);

  static std::unique_ptr<ParentConnection> MakeParentConnection(
      const ParentConnectionReceivedCallback& request_handler,
      const ParentConnection::ParentConnectionImpl::ClosedCallback&
          close_handler,
      const NodeThread& node_thread, int socket_fd, bool store_queries,
      bool use_locking);
  static void EnableParentConnection(ParentConnection& connection);

  static std::map<uint32_t, std::map<std::string, double>>
  MakeChildConnectionStatsMap(const ChildConnectionStats& stats,
                              double elapsed_time);
  static std::map<uint32_t, std::map<std::string, double>> MakeLeafNodeStatsMap(
      const LeafNodeStats& stats, double elapsed_time);
};
}  // namespace oldisim

#endif  // CONNECTION_UTIL_H
