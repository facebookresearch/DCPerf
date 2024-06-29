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

#ifndef OLDISIM_FAN_OUT_MANAGER_H
#define OLDISIM_FAN_OUT_MANAGER_H

#include <netdb.h>
#include <stdint.h>

#include <memory>
#include <functional>
#include <vector>

namespace oldisim {

class ChildConnection;
class ParentNodeServer;
class QueryContext;

struct FanoutRequest {
  uint32_t child_node_id;
  uint32_t request_type;
  const void* request_data;
  uint32_t request_data_length;
};

struct FanoutReply {
  bool timed_out;
  uint32_t child_node_id;
  uint32_t request_type;
  std::unique_ptr<uint8_t[]> reply_data;
  uint32_t reply_data_length;
  float latency_ms;
};

struct FanoutReplyTracker {
  uint64_t starting_request_id;
  int num_requests;
  int num_replies_received;
  std::vector<FanoutReply> replies;
  bool closed;  // Marked as such when timed out or when all replies received
  uint64_t start_time;
};

class FanoutManager {
  friend ParentNodeServer;

 public:
  // Methods to create child connections, used by user programs
  void MakeChildConnection(uint32_t child_node_id);
  void MakeChildConnections(uint32_t child_node_id, int num);

  // Methods to perform fanout queries
  typedef std::function<void(QueryContext&, const FanoutReplyTracker&)>
      FanoutDoneCallback;
  void Fanout(QueryContext&& originating_query, const FanoutRequest* requests,
              int num_requests, const FanoutDoneCallback& callback,
              double timeout_ms = 0.0);
  void FanoutAll(QueryContext&& originating_query, const FanoutRequest& request,
                 const FanoutDoneCallback& callback, double timeout_ms = 0.0);

 private:
  struct FanoutManagerImpl;
  std::unique_ptr<FanoutManagerImpl> impl_;
  explicit FanoutManager(std::unique_ptr<FanoutManagerImpl> impl);
};
}  // namespace oldisim

#endif  // OLDISIM_FAN_OUT_MANAGER_H

