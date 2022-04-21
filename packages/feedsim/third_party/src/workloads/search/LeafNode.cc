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

#include <chrono>
#include <memory>
#include <random>

#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Util.h"

#include "PointerChase.h"
#include "ICacheBuster.h"
#include "LeafNodeCmdline.h"
#include "RequestTypes.h"

// Shared configuration flags
static gengetopt_args_info args;

// Program constants
const int kPointerChaseSize = 10000000;
const int kICacheBusterSize = 100000;
const int kNumNops = 6;
const int kNumNopIterations = 60;
const int kNumIterations = 100000000;
const int kMaxResponseSize = 8192;

struct ThreadData {
  std::unique_ptr<search::PointerChase> pointer_chaser;
  std::unique_ptr<ICacheBuster> icache_buster;
  std::default_random_engine rng;
  std::gamma_distribution<double> latency_distribution;
  std::string random_string;
};

void ThreadStartup(oldisim::NodeThread& thread,
                   std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];

  // Initialize of I$Buster
  this_thread.pointer_chaser.reset(new search::PointerChase(kPointerChaseSize));

  // Initialize PointerChaser
  this_thread.icache_buster.reset(new ICacheBuster(kICacheBusterSize));

  // Initialize RNG and latency sampler
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  this_thread.rng.seed(seed);

  const double alpha = 0.7;
  const double beta = 20000;
  this_thread.latency_distribution =
      std::gamma_distribution<double>(alpha, beta);

  // Initialize random string with random bits
  this_thread.random_string = RandomString(kMaxResponseSize);
}

void SearchRequestHandler(oldisim::NodeThread& thread,
                          oldisim::QueryContext& context,
                          std::vector<ThreadData>& thread_data) {
  ThreadData& this_thread = thread_data[thread.get_thread_num()];
  search::PointerChase& chaser = *this_thread.pointer_chaser;
  ICacheBuster& buster = *this_thread.icache_buster;

  // Sample distribution for work
  int num_iterations = this_thread.latency_distribution(this_thread.rng);

  // Spin loop of work here
  for (int i = 0; i < num_iterations; i++) {
    buster.RunNextMethod();
    chaser.Chase(1);
    for (int j = 0; j < kNumNopIterations; j++) {
      for (int k = 0; k < kNumNops; k++) {
        asm volatile("nop");
      }
    }
  }

  int response_size = 2048;
  context.SendResponse(
      reinterpret_cast<const uint8_t*>(this_thread.random_string.c_str()),
      response_size);
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

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  oldisim::LeafNodeServer server(args.port_arg);

  server.SetThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::ref(thread_data)));
  server.RegisterQueryCallback(
      search::kSearchRequestType,
      std::bind(SearchRequestHandler, std::placeholders::_1,
                std::placeholders::_2,
                std::ref(thread_data)));

  server.SetNumThreads(args.threads_arg);
  server.SetThreadPinning(!args.noaffinity_given);
  server.SetThreadLoadBalancing(!args.noloadbalance_given);

  // Enable remote monitoring
  server.EnableMonitoring(args.monitor_port_arg);

  server.Run();

  return 0;
}
