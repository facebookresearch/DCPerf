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

#include <inttypes.h>
#include <event2/event.h>

#include <memory>
#include <vector>
#include <unordered_map>

#include "oldisim/Callbacks.h"
#include "oldisim/TestDriver.h"

namespace oldisim {

class ChildConnection;
class NodeThread;

struct TestDriver::TestDriverImpl {
  TestDriver& owner;
  // The below vector is "virtually" split into two parts
  // The first part is a list of ready connections that can send data
  // The second part is a list of connections that are backed up
  // (exceeded max connection depth)
  // The stored elements is a pair of connection ID, connection
  std::vector<std::pair<int, std::unique_ptr<ChildConnection>>> connections;
  // Below is a parallel array associating child connection ID to where
  // they are in the connections vector. Used to maintain the partitioning
  // between ready and non-ready connections
  std::vector<int> connection_positions;
  int next_connection_index;
  int num_ready_connections;
  int max_connection_depth;
  uint64_t next_request_id;
  NodeThread& node_thread;
  ChildConnectionStats current_child_stats;
  ChildConnectionStats last_child_stats;

  // Timer event to make the next request
  event* next_request_event;
  uint64_t next_request_delay_us;

  // Number of backlogged requests. A request is backlogged if it was scheduled
  // to be generated, but all connections were not ready at the time it
  // was to be generated.
  int num_backlogged_requests;

  // Callback pointers and data
  const std::unordered_map<uint32_t, const DriverNodeResponseCallback>&
      on_reply_cbs;
  const DriverNodeMakeRequestCallback& make_request_cb;

  TestDriverImpl(TestDriver& owner, const addrinfo* _service_node_addr,
                 int num_connections, int _max_connection_depth,
                 const std::unordered_map<
                     uint32_t, const DriverNodeResponseCallback>& _on_reply_cbs,
                 const std::set<uint32_t>& request_types,
                 const DriverNodeMakeRequestCallback& make_request_cb,
                 NodeThread& _node_thread);
  ~TestDriverImpl();
  int GetNextConnectionIndex();

  bool IsConnectionReady(int connection_id);
  void MarkConnectionReady(int connection_id);
  void MarkConnectionNotReady(int connection_id);

  static void ResponseCallback(TestDriver& driver, ResponseContext& response,
                               int connection_id);
  static void ChildConnectionClosedHandler(TestDriver& driver,
                                           const ChildConnection& conn,
                                           int connection_id);
  static void NextRequestCallback(evutil_socket_t listener, int16_t event,
                                  void* arg);

  static void MakeRequests(TestDriver& driver);
};
}  // namespace oldisim
