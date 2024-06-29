// Copyright 2015 Google Inc. All Rights Reserved.
// Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include "oldisim/FanoutManager.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ConnectionUtil.h"
#include "FanoutManagerImpl.h"
#include "oldisim/ChildConnection.h"
#include "oldisim/ResponseContext.h"
#include "oldisim/Util.h"

namespace oldisim {

/**
 *  Implementation details for FanoutManager
 */
FanoutManager::FanoutManager(std::unique_ptr<FanoutManagerImpl> impl)
    : impl_(move(impl)) {}

void FanoutManager::MakeChildConnection(uint32_t child_node_id) {
  assert(child_node_id < impl_->child_nodes.size());

  std::unique_ptr<ChildConnection> conn(ConnectionUtil::MakeChildConnection(
      std::bind(FanoutManager::FanoutManagerImpl::ResponseCallback,
                std::ref(*this), std::placeholders::_1),
      std::bind(FanoutManager::FanoutManagerImpl::ChildConnectionClosedHandler,
                std::ref(*this), std::placeholders::_1),
      impl_->node_thread, impl_->child_node_addr[child_node_id],
      *impl_->child_nodes[child_node_id].stats, false, true));
  impl_->child_nodes[child_node_id].connections.emplace_back(std::move(conn));
}

void FanoutManager::MakeChildConnections(uint32_t child_node_id, int num) {
  for (int i = 0; i < num; i++) {
    MakeChildConnection(child_node_id);
  }
}

void FanoutManager::Fanout(QueryContext&& originating_query,
                           const FanoutRequest* requests, int num_requests,
                           const FanoutDoneCallback& callback,
                           double timeout_ms) {
  // Allocate new internal tracker
  auto tracker = std::make_shared<FanoutReplyTrackerInternal>(
      impl_->next_request_id, num_requests, callback,
      std::move(originating_query));

  // Send the request out on the child connections, round-robin between
  // connections to the same child node
  for (int i = 0; i < num_requests; i++) {
    const FanoutRequest& request = requests[i];
    ChildConnection& conn = impl_->GetConnection(request.child_node_id);

    // Check if request type has been registered
    if (impl_->request_types.count(request.request_type) == 0) {
      DIE("Request type %d has not been registered\n", request.request_type);
    }

    conn.IssueRequest(request.request_type, impl_->next_request_id++,
                      request.request_data, request.request_data_length);

    // Fill in tracking data
    tracker->user_tracker.replies[i].child_node_id = request.child_node_id;
    tracker->user_tracker.replies[i].request_type = request.request_type;
  }

  // Register the tracker
  impl_->RegisterReplyTracker(tracker);

  tracker->user_tracker.start_time = GetTimeAccurateNano();

  // Activate timeout timer if specified
  if (timeout_ms > 0) {
    impl_->RegisterTrackerTimeout(tracker, timeout_ms);
  }
}

void FanoutManager::FanoutAll(QueryContext&& originating_query,
                              const FanoutRequest& request,
                              const FanoutDoneCallback& callback,
                              double timeout_ms) {
  // Check if request type has been registered
  if (impl_->request_types.count(request.request_type) == 0) {
    DIE("Request type %d has not been registered\n", request.request_type);
  }

  // Allocate new internal tracker
  auto tracker = std::make_shared<FanoutReplyTrackerInternal>(
      impl_->next_request_id, impl_->child_nodes.size(), callback,
      std::move(originating_query));

  // Send the request out on the child connections, round-robin between
  // connections to the same child node
  for (int i = 0; i < impl_->child_nodes.size(); i++) {
    ChildConnection& conn = impl_->GetConnection(i);
    conn.IssueRequest(request.request_type, impl_->next_request_id++,
                      request.request_data, request.request_data_length);

    // Fill in tracking data
    tracker->user_tracker.replies[i].child_node_id = i;
    tracker->user_tracker.replies[i].request_type = request.request_type;
  }

  // Register the tracker
  impl_->RegisterReplyTracker(tracker);

  tracker->user_tracker.start_time = GetTimeAccurateNano();

  // Activate timeout timer if specified
  if (timeout_ms > 0) {
    impl_->RegisterTrackerTimeout(tracker, timeout_ms);
  }
}

/**
 *  Implementation details for FanoutManagerImpl
 */
FanoutManager::FanoutManagerImpl::FanoutManagerImpl(
    const std::vector<addrinfo*>& _child_node_addr,
    const std::set<uint32_t>& _request_types, const NodeThread& _node_thread)
    : child_node_addr(_child_node_addr),
      next_request_id(0),
      request_types(_request_types),
      node_thread(_node_thread) {
  child_nodes.resize(child_node_addr.size());

  // Create connection stats objects for each node
  for (int i = 0; i < child_nodes.size(); i++) {
    child_nodes[i].stats.reset(new ChildConnectionStats(request_types));
  }
}

ChildConnection& FanoutManager::FanoutManagerImpl::GetConnection(
    uint32_t child_node_id) {
  assert(child_node_id < child_nodes.size());
  assert(child_nodes[child_node_id].connections.size() > 0);

  const auto& connection_ptr =
      child_nodes[child_node_id]
          .connections[child_nodes[child_node_id].next_connection_index];
  child_nodes[child_node_id].next_connection_index =
      (child_nodes[child_node_id].next_connection_index + 1) %
      child_nodes[child_node_id].connections.size();

  return *connection_ptr;
}

void FanoutManager::FanoutManagerImpl::RegisterReplyTracker(
    std::shared_ptr<FanoutReplyTrackerInternal> tracker) {
  for (int i = 0; i < tracker->user_tracker.num_requests; i++) {
    uint64_t request_id = tracker->user_tracker.starting_request_id + i;
    assert(tracker_by_id.count(request_id) == 0);
    tracker_by_id[request_id] = tracker;
  }
}

void FanoutManager::FanoutManagerImpl::UnregisterReplyTracker(
    const FanoutReplyTrackerInternal& tracker) {
  const uint32_t num_requests = tracker.user_tracker.num_requests;
  const uint64_t starting_request_id = tracker.user_tracker.starting_request_id;
  for (int i = 0; i < num_requests; i++) {
    uint64_t request_id = starting_request_id + i;
    assert(tracker_by_id.count(request_id) == 1);
    assert(tracker_by_id.at(request_id).get() == &tracker);
    tracker_by_id.erase(request_id);
  }
}

void FanoutManager::FanoutManagerImpl::RegisterTrackerTimeout(
    const std::shared_ptr<FanoutReplyTrackerInternal>& tracker,
    double timeout_ms) {
  // Make a heap-allocated shared_ptr to not leak the tracker
  auto timeout_args = new TimeoutArgs(*this, tracker);
  tracker->timeout_event = evtimer_new(
      node_thread.get_event_base(),
      FanoutManager::FanoutManagerImpl::TimeoutCallback, timeout_args);
  timeval tv;
  DoubleToTv(timeout_ms / 1000, &tv);
  evtimer_add(tracker->timeout_event, &tv);
}

void FanoutManager::FanoutManagerImpl::CloseTracker(
    FanoutManager& manager, FanoutReplyTrackerInternal& tracker) {
  CloseTracker(*manager.impl_, tracker);
}

void FanoutManager::FanoutManagerImpl::CloseTracker(
    FanoutManagerImpl& manager_impl, FanoutReplyTrackerInternal& tracker) {
  tracker.user_tracker.closed = true;  // Close the tracker
  tracker.done_callback(tracker.originating_query,
                        tracker.user_tracker);  // Call user-callback
  if (tracker.timeout_event != nullptr) {
    // Deregister timeout event
    auto timeout_args = reinterpret_cast<TimeoutArgs*>(
        event_get_callback_arg(tracker.timeout_event));
    delete timeout_args;
    event_free(tracker.timeout_event);
    tracker.timeout_event = nullptr;
  }
  // Have to do this last, because when the last one is deleted the
  // reference becomes no-good (shared-ptr) will clean it up from under you
  manager_impl.UnregisterReplyTracker(tracker);  // remove from tracker table
}

void FanoutManager::FanoutManagerImpl::ResponseCallback(
    FanoutManager& manager, ResponseContext& context) {
  // Get the tracking data structure in the map
  const auto& tracker_it =
      manager.impl_->tracker_by_id.find(context.request_id);
  if (tracker_it == manager.impl_->tracker_by_id.end()) {
    return;
  } else {
    FanoutReplyTrackerInternal& tracker = *tracker_it->second;

    // Debug checking
    assert(context.request_id >= tracker.user_tracker.starting_request_id);
    assert(context.request_id < tracker.user_tracker.starting_request_id +
                                    tracker.user_tracker.num_requests);

    // Find the index in the tracker holding the reply struct
    int index = context.request_id - tracker.user_tracker.starting_request_id;
    FanoutReply& reply = tracker.user_tracker.replies[index];
    assert(reply.request_type == context.type);
    assert(reply.reply_data == nullptr);

    // Fill in the fields of the reply object
    reply.timed_out = false;
    // Allocate memory to copy the payload data
    uint8_t* payload_copy = new uint8_t[context.payload_length];
    memcpy(payload_copy, context.payload, context.payload_length);
    reply.reply_data = std::unique_ptr<uint8_t[]>(payload_copy);
    reply.reply_data_length = context.payload_length;
    reply.latency_ms =
        (context.response_timestamp - context.request_timestamp) / 1000000.0;

    // Update tracker, check to see if all responses received
    tracker.user_tracker.num_replies_received++;
    if (tracker.user_tracker.num_replies_received ==
        tracker.user_tracker.num_requests) {
      CloseTracker(manager, tracker);
    }
  }
}

void FanoutManager::FanoutManagerImpl::ChildConnectionClosedHandler(
    const FanoutManager& manager, const ChildConnection& conn) {}

void FanoutManager::FanoutManagerImpl::TimeoutCallback(evutil_socket_t listener,
                                                       int16_t event,
                                                       void* arg) {
  auto args = reinterpret_cast<TimeoutArgs*>(arg);
  FanoutManager::FanoutManagerImpl& manager_impl = std::get<0>(*args);
  auto tracker = std::get<1>(*args);

  // Log timed out requests
  for (const auto& reply_tracker : tracker->user_tracker.replies) {
    if (reply_tracker.timed_out) {
      manager_impl.child_nodes[reply_tracker.child_node_id]
          .stats->LogDroppedRequest(reply_tracker.request_type);
    }
  }

  CloseTracker(manager_impl, *tracker);
}

/**
 * Implementation details of FanoutReplyTrackerInternal
 */
static FanoutReply EmptyFanoutReply() {
  FanoutReply empty;
  empty.timed_out = true;
  empty.child_node_id = 0;
  empty.request_type = 0;
  empty.reply_data = nullptr;
  empty.reply_data_length = 0;
  empty.latency_ms = 0.0f;

  return empty;
}

FanoutReplyTrackerInternal::FanoutReplyTrackerInternal(
    uint64_t starting_request_id, int num_requests,
    const FanoutManager::FanoutDoneCallback& _done_callback,
    QueryContext&& _originating_query)
    : done_callback(_done_callback),
      timeout_event(nullptr),
      originating_query(std::move(_originating_query)) {
  // Initialize user_tracker
  user_tracker.starting_request_id = starting_request_id;
  user_tracker.num_requests = num_requests;
  user_tracker.num_replies_received = 0;
  user_tracker.closed = false;

  // Fill up FanoutReply vector
  user_tracker.replies.reserve(num_requests);
  for (int i = 0; i < num_requests; i++) {
    user_tracker.replies.emplace_back(EmptyFanoutReply());
  }
}

FanoutReplyTrackerInternal::~FanoutReplyTrackerInternal() {
  if (timeout_event != nullptr) {
    event_free(timeout_event);
    timeout_event = nullptr;
  }
}
}  // namespace oldisim

