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

#include <stdio.h>
#include <string.h>

#include <memory>

#include "oldisim/FanoutManager.h"
#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/ParentNodeServer.h"
#include "oldisim/QueryContext.h"

const int kNumLeafs = 4;

// Declarations of handlers
void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::FanoutManager& fanout_manager, const void* data);
void AcceptHandler(oldisim::NodeThread& thread, oldisim::ParentConnection& conn,
                   const void* data);
void Type0Handler(oldisim::NodeThread& thread,
                  oldisim::FanoutManager& fanout_manager,
                  oldisim::QueryContext& context, const void* data);
void Type0Handler_part1(oldisim::NodeThread& thread,
                        oldisim::FanoutManager& fanout_manager,
                        oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results);
void Type0Handler_part2(oldisim::NodeThread& thread,
                        oldisim::FanoutManager& fanout_manager,
                        oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results);
void Type0Handler_part3(oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results);

void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::FanoutManager& fanout_manager, const void* data) {
  // Create child connections
  for (int i = 0; i < kNumLeafs; i++) {
    fanout_manager.MakeChildConnections(i, 24);
  }
}

void AcceptHandler(oldisim::NodeThread& thread, oldisim::ParentConnection& conn,
                   const void* data) {
  printf("Got new connection\n");
  printf("pthread id: %08x\n", static_cast<int>(thread.get_pthread()));
  printf("event_base: %p\n", thread.get_event_base());
  printf("conn: %p\n", &conn);
  printf("Interpreting data as string: %s\n", reinterpret_cast<char*>(
                                              const_cast<void*>(data)));
}

void Type0Handler(oldisim::NodeThread& thread,
                  oldisim::FanoutManager& fanout_manager,
                  oldisim::QueryContext& context, const void* data) {
  // Set up fanout structure to everyone
  oldisim::FanoutRequest request;
  request.request_type = 0;
  request.request_data = nullptr;
  request.request_data_length = 0;

  fanout_manager.FanoutAll(std::move(context), request,
                           std::bind(Type0Handler_part1, std::ref(thread),
                                     std::ref(fanout_manager),
                                     std::placeholders::_1,
                                     std::placeholders::_2));
}

void Type0Handler_part1(oldisim::NodeThread& thread,
                        oldisim::FanoutManager& fanout_manager,
                        oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results) {
  oldisim::FanoutRequest requests[] = {
      {0, 0, nullptr, 0}, {2, 0, nullptr, 0}, {4, 0, nullptr, 0}};

  fanout_manager.Fanout(std::move(originating_query), requests, 3,
                        std::bind(Type0Handler_part2, std::ref(thread),
                                  std::ref(fanout_manager),
                                  std::placeholders::_1,
                                  std::placeholders::_2),
                        3.0);
}

void Type0Handler_part2(oldisim::NodeThread& thread,
                        oldisim::FanoutManager& fanout_manager,
                        oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results) {
  oldisim::FanoutRequest requests[] = {{1, 0, nullptr, 0}, {3, 0, nullptr, 0}};

  fanout_manager.Fanout(std::move(originating_query), requests, 2,
                        Type0Handler_part3, 3.0);
}

void Type0Handler_part3(oldisim::QueryContext& originating_query,
                        const oldisim::FanoutReplyTracker& results) {
  // Finally send back the data
  static const char test_response[] =
      "i am a test response for 0 "
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst"
      "uvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  originating_query.SendResponse(
      reinterpret_cast<const uint8_t*>(test_response), sizeof(test_response));
}

int main() {
  oldisim::ParentNodeServer server(11223);

  const char* test_string1 = "hi hi hi";
  const char* test_string2 = "yay yay yay";
  const char* test_string3 = "handler handler handler000";
  const char* test_string4 = "handler handler handler222";
  const char* test_string5 = "handler handler handler8888";

  server.SetThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                test_string1));

  server.RegisterQueryCallback(
      0, std::bind(Type0Handler, std::placeholders::_1, std::placeholders::_2,
                   std::placeholders::_3, test_string3));

  server.RegisterRequestType(0);

  // Add some child nodes
  server.AddChildNode("localhost", 11222);
  server.AddChildNode("localhost", 11222);
  server.AddChildNode("localhost", 11222);
  server.AddChildNode("localhost", 11222);

  // Enable remote monitoring
  server.EnableMonitoring(8888);

  server.Run(24, true);

  return 0;
}
