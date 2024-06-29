// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>

#include "oldisim/ChildConnectionStats.h"
#include "oldisim/DriverNode.h"
#include "oldisim/Log.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ResponseContext.h"
#include "oldisim/TestDriver.h"
#include "oldisim/Util.h"

#include "DriverNodeRankCmdline.h"
#include "RequestTypes.h"

#include "utils.h"

static gengetopt_args_info args;

const int kMaxRequestSize = 8192;
const int kRecomputeQPSPeriod = 5;

struct ThreadData {
  std::string random_string;
  double qps_per_thread;
  uint64_t request_delay; // This is per thread
  oldisim::TestDriver *test_driver;
  event *recompute_qps_timer;
};

// Specific timer handler to recompute inter-request delays for QPS
void AddRecomputeDelayTimer(ThreadData &this_thread);
void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg);

// Declarations of handlers
void ThreadStartup(oldisim::NodeThread &thread,
                   oldisim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data);
void MakeRequest(oldisim::NodeThread &thread, oldisim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data);

void AddRecomputeDelayTimer(ThreadData &this_thread) {
  timeval t = {kRecomputeQPSPeriod, 0};
  evtimer_add(this_thread.recompute_qps_timer, &t);
}

void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg) {
  ThreadData *this_thread = reinterpret_cast<ThreadData *>(arg);
  const oldisim::ChildConnectionStats &stats =
      this_thread->test_driver->GetConnectionStats();

  // Get QPS for last stats period
  double qps = static_cast<double>(
                   stats.query_counts_.at(ranking::kPageRankRequestType)) /
               (stats.end_time_ - stats.start_time_) * 1000000000;

  // Adjust delay based on QPS
  this_thread->request_delay = (1000000 / this_thread->qps_per_thread) * (qps / this_thread->qps_per_thread);

  AddRecomputeDelayTimer(*this_thread);
}

void ThreadStartup(oldisim::NodeThread &thread,
                   oldisim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread.get_thread_num()];

  // Initialize random string with random bits
  this_thread.random_string = RandomString(kMaxRequestSize);

  // Store pointer to test_driver
  this_thread.test_driver = &test_driver;

  // If user gave QPS target, initialize QPS modulation
  if (args.qps_arg != 0) {
    this_thread.qps_per_thread =
        (static_cast<double>(args.qps_arg)) / args.threads_arg;
    this_thread.recompute_qps_timer = evtimer_new(
        thread.get_event_base(), RecomputeDelayTimerHandler, &this_thread);
    AddRecomputeDelayTimer(this_thread);
    this_thread.request_delay = 1000000 / this_thread.qps_per_thread;
  } else {
    this_thread.request_delay = 0;
  }
}

void MakeRequest(oldisim::NodeThread &thread, oldisim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread.get_thread_num()];

  test_driver.SendRequest(ranking::kPageRankRequestType,
                          this_thread.random_string.c_str(), 3000,
                          this_thread.request_delay);
}

int main(int argc, char **argv) {
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

  // Check requried arguments
  if (!args.server_given) {
    DIE("--server must be specified.");
  }

  auto host_port = ranking::utils::parseHostnameAndPort(args.server_arg);

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  oldisim::DriverNode driver_node(host_port.first, host_port.second);

  driver_node.SetThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));
  driver_node.SetMakeRequestCallback(
      std::bind(MakeRequest, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));
  driver_node.RegisterRequestType(ranking::kPageRankRequestType);

  // Enable remote monitoring
  driver_node.EnableMonitoring(args.monitor_port_arg);

  driver_node.Run(args.threads_arg, args.affinity_given, args.connections_arg,
                  args.depth_arg);

  return 0;
}
