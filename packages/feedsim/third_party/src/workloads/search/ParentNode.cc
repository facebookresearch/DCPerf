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

#include <stdio.h>
#include <string.h>

#include <memory>

#include "oldisim/FanoutManager.h"
#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/ParentNodeServer.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Util.h"

#include "ParentNodeCmdline.h"
#include "RequestTypes.h"
#include "Util.h"

// Shared configuration flags
static gengetopt_args_info args;

// Program constants
const int kMaxLeafRequestSize = 8 * 1024;
const int kMaxResponseSize = 512 * 1024;

struct ThreadData {
  std::string random_string;
};

// Declarations of handlers
void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::FanoutManager& fanout_manager,
                   std::vector<ThreadData>& thread_data);
void SearchRequestHandler(oldisim::NodeThread& thread,
                          oldisim::FanoutManager& fanout_manager,
                          oldisim::QueryContext& context,
                          std::vector<ThreadData>& thread_data);
void SearchRequestFanoutDone(oldisim::QueryContext& originating_query,
                             const oldisim::FanoutReplyTracker& results,
                             ThreadData& this_thread);

void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::FanoutManager& fanout_manager,
                   std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];

  // Create child connections
  for (int i = 0; i < args.leaf_given; i++) {
    fanout_manager.MakeChildConnections(i, args.connections_arg);
  }

  // Initialize random string with random bits
  this_thread.random_string = RandomString(kMaxResponseSize);
}

void SearchRequestHandler(oldisim::NodeThread& thread,
                          oldisim::FanoutManager& fanout_manager,
                          oldisim::QueryContext& context,
                          std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];

  // Set up fanout structure to everyone
  oldisim::FanoutRequest request;
  request.request_type = search::kSearchRequestType;
  request.request_data = this_thread.random_string.c_str();
  request.request_data_length = 3500;

  fanout_manager.FanoutAll(
      std::move(context), request,
      std::bind(SearchRequestFanoutDone, std::placeholders::_1,
                std::placeholders::_2, std::ref(this_thread)));
}

void SearchRequestFanoutDone(oldisim::QueryContext& originating_query,
                             const oldisim::FanoutReplyTracker& results,
                             ThreadData& this_thread) {
  // Finally send back the data
  originating_query.SendResponse(
      reinterpret_cast<const uint8_t*>(this_thread.random_string.c_str()),
      15000);
}

int main(int argc, char** argv) {
  // Parse arguments
  if (cmdline_parser(argc, argv, &args) != 0) {
    DIE("cmdline_parser failed");
  }

  // Set logging level
  for (unsigned int i = 0; i < args.verbose_given; i++) {
    log_level = (log_level_t)(static_cast<int>(log_level) - 1);
  }
  if (args.quiet_given) {
    log_level = QUIET;
  }

  // Check for leaf servers
  if (args.leaf_given == 0) {
    DIE("--leaf must be specified.");
  }

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  oldisim::ParentNodeServer server(args.port_arg);

  server.SetThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));
  server.RegisterQueryCallback(
      search::kSearchRequestType,
      std::bind(SearchRequestHandler, std::placeholders::_1,
                std::placeholders::_2, std::placeholders::_3,
                std::ref(thread_data)));
  server.RegisterRequestType(search::kSearchRequestType);

  // Add leaf nodes
  for (int i = 0; i < args.leaf_given; i++) {
    std::string hostname;
    int port;
    search::ParseServerAddress(args.leaf_arg[i], hostname, port);

    server.AddChildNode(hostname, port);
  }

  // Enable remote monitoring
  server.EnableMonitoring(args.monitor_port_arg);

  server.Run(args.threads_arg, true);

  return 0;
}

