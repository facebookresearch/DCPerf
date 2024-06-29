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

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>

#include "oldisim/FanoutManager.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/ParentNodeServer.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Util.h"

#include "LoadBalancerNodeCmdline.h"
#include "RequestTypes.h"
#include "Util.h"

// Shared configuration flags
static gengetopt_args_info args;

// Program constants
const int kMaxLeafRequestSize = 8 * 1024;
const int kMaxResponseSize = 512 * 1024;

struct ThreadData {
  std::default_random_engine rng;
  std::vector<int> parent_connection_depths;
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
                             ThreadData& this_thread, int parent_node_index);

void ThreadStartup(oldisim::NodeThread& thread,
                   oldisim::FanoutManager& fanout_manager,
                   std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];

  // Create child connections
  for (int i = 0; i < args.parent_given; i++) {
    fanout_manager.MakeChildConnections(i, args.connections_arg);
  }

  // Initialize per-thread RNG
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count() +
                  thread.get_thread_num();
  this_thread.rng.seed(seed);

  // Create # of outstanding requests structure for load balancing
  this_thread.parent_connection_depths.resize(args.parent_given);
}

void SearchRequestHandler(oldisim::NodeThread& thread,
                          oldisim::FanoutManager& fanout_manager,
                          oldisim::QueryContext& context,
                          std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];

  // Find least loaded parent
  int parent_node_index =
      std::min_element(this_thread.parent_connection_depths.begin(),
                       this_thread.parent_connection_depths.end()) -
      this_thread.parent_connection_depths.begin();

  // Set up fanout structure to everyone
  oldisim::FanoutRequest request;
  request.child_node_id =
      parent_node_index;  // this_thread.rng() % args.parent_given;
  request.request_type = search::kSearchRequestType;
  request.request_data = context.payload;
  request.request_data_length = context.payload_length;

  fanout_manager.Fanout(std::move(context), &request, 1,
                        std::bind(SearchRequestFanoutDone,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::ref(this_thread), parent_node_index));
  this_thread.parent_connection_depths[parent_node_index]++;
}

void SearchRequestFanoutDone(oldisim::QueryContext& originating_query,
                             const oldisim::FanoutReplyTracker& results,
                             ThreadData& this_thread, int parent_node_index) {
  // Finally send back the data
  originating_query.SendResponse(results.replies[0].reply_data.get(),
                                 results.replies[0].reply_data_length);
  this_thread.parent_connection_depths[parent_node_index]--;
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

  // Check for parent servers
  if (args.parent_given == 0) {
    DIE("--parent must be specified.");
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

  // Add parent nodes
  for (int i = 0; i < args.parent_given; i++) {
    std::string hostname;
    int port;
    search::ParseServerAddress(args.parent_arg[i], hostname, port);

    server.AddChildNode(hostname, port);
  }

  // Enable remote monitoring
  server.EnableMonitoring(args.monitor_port_arg);

  server.Run(args.threads_arg, true);

  return 0;
}
