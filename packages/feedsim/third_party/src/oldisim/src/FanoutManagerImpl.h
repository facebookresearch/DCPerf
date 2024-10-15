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
#pragma once

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include <set>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "oldisim/ChildConnectionStats.h"
#include "oldisim/FanoutManager.h"
#include "oldisim/QueryContext.h"

namespace oldisim {

class ChildConnection;
class FanoutManager;
class NodeThread;

struct FanoutReplyTrackerInternal {
  FanoutReplyTracker user_tracker;
  FanoutManager::FanoutDoneCallback done_callback;
  event* timeout_event;
  QueryContext originating_query;

  FanoutReplyTrackerInternal(
      uint64_t starting_request_id, int num_requests,
      const FanoutManager::FanoutDoneCallback& _done_callback,
      QueryContext&& _originating_query);
  ~FanoutReplyTrackerInternal();
};

struct FanoutNode {
  std::vector<std::unique_ptr<ChildConnection>> connections;
  uint32_t next_connection_index;
  std::unique_ptr<ChildConnectionStats> stats;
};

struct FanoutManager::FanoutManagerImpl {
  const std::vector<addrinfo*>& child_node_addr;
  std::vector<FanoutNode> child_nodes;
  uint64_t next_request_id;
  const std::set<uint32_t>& request_types;
  const NodeThread& node_thread;

  // Facilitie to keep track of outstanding requests
  std::unordered_map<uint64_t, std::shared_ptr<FanoutReplyTrackerInternal>>
      tracker_by_id;

  FanoutManagerImpl(const std::vector<addrinfo*>& _child_node_addr,
                    const std::set<uint32_t>& _request_types,
                    const NodeThread& _node_thread);
  ChildConnection& GetConnection(uint32_t child_node_id);

  // Methods to register/deregister a tracker
  void RegisterReplyTracker(
      std::shared_ptr<FanoutReplyTrackerInternal> tracker);
  void UnregisterReplyTracker(const FanoutReplyTrackerInternal& tracker);
  void RegisterTrackerTimeout(
      const std::shared_ptr<FanoutReplyTrackerInternal>& tracker,
      double timeout_ms);

  // Helper method when closing a tracker
  static void CloseTracker(FanoutManager& manager,
                           FanoutReplyTrackerInternal& tracker);
  static void CloseTracker(FanoutManagerImpl& manager_impl,
                           FanoutReplyTrackerInternal& tracker);

  static void ResponseCallback(FanoutManager& manager,
                               ResponseContext& response);
  static void ChildConnectionClosedHandler(const FanoutManager& manager,
                                           const ChildConnection& conn);
  static void TimeoutCallback(evutil_socket_t listener, int16_t event,
                              void* arg);

  // Typedef for timeout argument
  typedef std::tuple<FanoutManagerImpl&,
                     std::shared_ptr<FanoutReplyTrackerInternal>> TimeoutArgs;
};
}  // namespace oldisim
