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

#include "oldisim/TestDriver.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ConnectionUtil.h"
#include "TestDriverImpl.h"
#include "oldisim/Callbacks.h"
#include "oldisim/ChildConnection.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ResponseContext.h"
#include "oldisim/Util.h"

namespace oldisim {

/**
 * Implementation details for TestDriver
 */
void TestDriver::Start() { TestDriverImpl::MakeRequests(*this); }

void TestDriver::SendRequest(uint32_t type, const void* payload,
                             uint32_t payload_length,
                             uint64_t next_request_delay_us) {
  int index = impl_->GetNextConnectionIndex();
  int conn_id = impl_->connections[index].first;
  auto& conn = *impl_->connections[index].second;
  conn.IssueRequest(type, impl_->next_request_id++, payload, payload_length);

  // Check to see if this connection is filled to max depth
  if (conn.GetNumOutstandingRequests() == impl_->max_connection_depth) {
    impl_->MarkConnectionNotReady(conn_id);
  }

  // Schedule next request
  impl_->next_request_delay_us = next_request_delay_us;

  // Only create next query timer if it needs to be scheduled
  if (next_request_delay_us != 0) {
    timeval tv;
    MicroToTv(next_request_delay_us, &tv);
    evtimer_add(impl_->next_request_event, &tv);
  }
}

const ChildConnectionStats& TestDriver::GetConnectionStats() const {
  return impl_->last_child_stats;
}

TestDriver::TestDriver() : impl_(nullptr) {}

/**
 * Implementation details for TestDriverImpl
 */
TestDriver::TestDriverImpl::TestDriverImpl(
    TestDriver& _owner, const addrinfo* _service_node_addr, int num_connections,
    int _max_connection_depth,
    const std::unordered_map<uint32_t, const DriverNodeResponseCallback>&
        _on_reply_cbs,
    const std::set<uint32_t>& request_types,
    const DriverNodeMakeRequestCallback& _make_request_cb,
    NodeThread& _node_thread)
    : owner(_owner),
      max_connection_depth(_max_connection_depth),
      on_reply_cbs(_on_reply_cbs),
      make_request_cb(_make_request_cb),
      next_request_id(0),
      node_thread(_node_thread),
      current_child_stats(request_types),
      last_child_stats(request_types),
      next_request_event(nullptr),
      next_request_delay_us(0),
      num_backlogged_requests(0) {
  // Establish connections to the service
  connections.reserve(num_connections);
  connection_positions.reserve(num_connections);
  for (int i = 0; i < num_connections; i++) {
    std::unique_ptr<ChildConnection> conn(ConnectionUtil::MakeChildConnection(
        std::bind(TestDriver::TestDriverImpl::ResponseCallback, std::ref(owner),
                  std::placeholders::_1, i),
        std::bind(TestDriver::TestDriverImpl::ChildConnectionClosedHandler,
                  std::ref(owner), std::placeholders::_1, i),
        node_thread, _service_node_addr, current_child_stats, false, true));
    connections.emplace_back(std::make_pair(i, std::move(conn)));
    connection_positions.push_back(i);
  }

  next_connection_index = 0;
  num_ready_connections = num_connections;

  // Make timer for next query, but do not activate yet
  next_request_event =
      evtimer_new(node_thread.get_event_base(), NextRequestCallback, &owner);
}

TestDriver::TestDriverImpl::~TestDriverImpl() {
  if (next_request_event != nullptr) {
    event_free(next_request_event);
  }
}

int TestDriver::TestDriverImpl::GetNextConnectionIndex() {
  if (num_ready_connections == 0) {
    return -1;
  } else {
    assert(next_connection_index < num_ready_connections);
    return next_connection_index;
  }
}

bool TestDriver::TestDriverImpl::IsConnectionReady(int connection_id) {
  return connection_positions[connection_id] < num_ready_connections;
}

void TestDriver::TestDriverImpl::MarkConnectionReady(int connection_id) {
  assert(!IsConnectionReady(connection_id));
  int position = connection_positions[connection_id];
  int new_position = num_ready_connections;
  int swapped_connection_id = connections[new_position].first;
  std::swap(connections[new_position], connections[position]);
  std::swap(connection_positions[swapped_connection_id],
            connection_positions[connection_id]);
  num_ready_connections++;
}

void TestDriver::TestDriverImpl::MarkConnectionNotReady(int connection_id) {
  assert(IsConnectionReady(connection_id));
  int position = connection_positions[connection_id];
  int new_position = num_ready_connections - 1;
  int swapped_connection_id = connections[new_position].first;
  std::swap(connections[new_position], connections[position]);
  std::swap(connection_positions[swapped_connection_id],
            connection_positions[connection_id]);
  num_ready_connections--;
}

void TestDriver::TestDriverImpl::ResponseCallback(TestDriver& driver,
                                                  ResponseContext& response,
                                                  int connection_id) {
  const auto& callback = driver.impl_->on_reply_cbs.find(response.type);
  if (callback != driver.impl_->on_reply_cbs.end()) {
    callback->second(driver.impl_->node_thread, response);
  }

  // Check to see if connection was previously backlogged, if so, mark it as
  // ready
  if (!driver.impl_->IsConnectionReady(connection_id)) {
    driver.impl_->MarkConnectionReady(connection_id);
  }

  // If there were backlogged queries, run the user query generator
  if (driver.impl_->num_backlogged_requests) {
    MakeRequests(driver);
    driver.impl_->num_backlogged_requests--;
  }
}

void TestDriver::TestDriverImpl::ChildConnectionClosedHandler(
    TestDriver& driver, const ChildConnection& conn, int connection_id) {
  // Check to see if this connection was previously ready
  if (driver.impl_->IsConnectionReady(connection_id)) {
    driver.impl_->MarkConnectionNotReady(connection_id);
  }
}

void TestDriver::TestDriverImpl::NextRequestCallback(evutil_socket_t listener,
                                                     int16_t event, void* arg) {
  TestDriver* driver = reinterpret_cast<TestDriver*>(arg);
  MakeRequests(*driver);
}

void TestDriver::TestDriverImpl::MakeRequests(TestDriver& driver) {
  // Optimization: if delay is 0, then generate queries as fast as possible
  do {
    // Check to see if there are any ready connections, if not, then
    // the driver is backlogged
    if (driver.impl_->num_ready_connections == 0) {
      driver.impl_->num_backlogged_requests++;
      // Don't call the callback, wait until connections drain later
      return;
    } else {
      driver.impl_->make_request_cb(driver.impl_->node_thread, driver);
    }
  } while (driver.impl_->next_request_delay_us == 0);
}
}  // namespace oldisim
