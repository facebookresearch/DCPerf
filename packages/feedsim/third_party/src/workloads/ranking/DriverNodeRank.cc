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

#include <algorithm>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#include <event2/event.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "FeedSimDriver.h"
#include "FeedSimProtocol.h"

#include "DriverNodeRankCmdline.h"
#include "FeatureGenerator.h"
#include "RequestTypes.h"

#include "if/gen-cpp2/ranking_types.h"

#include "utils.h"

static gengetopt_args_info args;

const int kMaxRequestSize = 8192;
const int kRecomputeQPSPeriod = 1;  // Reduced from 5 to 1 second for faster feedback

// Simple random string generator (replaces oldisim/Util.h RandomString)
static std::string RandomString(size_t length) {
  static const char charset[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
  std::string str(length, 0);
  for (size_t i = 0; i < length; ++i) {
    str[i] = charset[dist(rng)];
  }
  return str;
}

struct ThreadData {
  std::string random_string;
  double qps_per_thread;
  uint64_t request_delay; // This is per thread
  feedsim::TestDriver *test_driver;
  event *recompute_qps_timer;

  // Client-side feature generation
  std::unique_ptr<ranking::FeatureGenerator> feature_generator;
  std::string serialized_request;  // Pre-allocated buffer for serialized request
};

// Specific timer handler to recompute inter-request delays for QPS
void AddRecomputeDelayTimer(ThreadData &this_thread);
void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg);

// Declarations of handlers
void ThreadStartup(int thread_id,
                   feedsim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data);
void MakeRequest(int thread_id, feedsim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data);

void AddRecomputeDelayTimer(ThreadData &this_thread) {
  timeval t = {kRecomputeQPSPeriod, 0};
  evtimer_add(this_thread.recompute_qps_timer, &t);
}

void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg) {
  ThreadData *this_thread = reinterpret_cast<ThreadData *>(arg);
  const feedsim::DriverStats &stats =
      this_thread->test_driver->getConnectionStats();

  // Get QPS for last stats period
  uint64_t now = feedsim::getTimeNano();
  double elapsed_secs = (now - stats.getStartTimeNano()) / 1000000000.0;
  if (elapsed_secs <= 0) {
    AddRecomputeDelayTimer(*this_thread);
    return;
  }

  // Use the appropriate request type for QPS calculation
  uint32_t request_type = args.client_side_features_given
      ? ranking::kDLRMRequestType
      : ranking::kPageRankRequestType;

  double measured_qps = static_cast<double>(stats.getQueryCount(request_type))
                        / elapsed_secs;

  // Compute target delay in microseconds
  double target_delay_us = 1000000.0 / this_thread->qps_per_thread;

  // Adjust delay using proportional feedback control
  // If measured_qps > target: increase delay to slow down
  // If measured_qps < target: decrease delay to speed up
  // Use a damping factor (0.5) to prevent oscillations
  double qps_ratio = measured_qps / this_thread->qps_per_thread;
  double damping = 0.5;
  double adjustment = 1.0 + damping * (qps_ratio - 1.0);

  // Clamp adjustment to prevent extreme values
  if (adjustment < 0.5) adjustment = 0.5;
  if (adjustment > 2.0) adjustment = 2.0;

  this_thread->request_delay = static_cast<uint64_t>(target_delay_us * adjustment);

  // Ensure minimum delay to prevent flooding
  if (this_thread->request_delay < 100) {
    this_thread->request_delay = 100;  // 100us minimum = 10000 QPS max per thread
  }

  AddRecomputeDelayTimer(*this_thread);
}

void ThreadStartup(int thread_id,
                   feedsim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread_id];

  // Initialize random string with random bits
  this_thread.random_string = RandomString(kMaxRequestSize);

  // Store pointer to test_driver
  this_thread.test_driver = &test_driver;

  // Initialize client-side feature generator if enabled
  if (args.client_side_features_given) {
    ranking::FeatureGeneratorConfig config;
    config.batch_size = args.client_dlrm_batch_size_arg;
    config.num_dense_features = args.client_num_dense_features_arg;
    config.num_sparse_features = args.client_num_sparse_features_arg;
    config.seed = static_cast<unsigned>(args.client_feature_seed_arg);

    this_thread.feature_generator =
        std::make_unique<ranking::FeatureGenerator>(config, thread_id);
  }

  // If user gave QPS target, initialize QPS modulation
  if (args.qps_arg != 0) {
    this_thread.qps_per_thread =
        (static_cast<double>(args.qps_arg)) / args.threads_arg;
    this_thread.recompute_qps_timer = evtimer_new(
        test_driver.getEventBase(), RecomputeDelayTimerHandler, &this_thread);
    AddRecomputeDelayTimer(this_thread);
    this_thread.request_delay = 1000000 / this_thread.qps_per_thread;
  } else {
    this_thread.request_delay = 0;
  }
}

void MakeRequest(int thread_id, feedsim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread_id];

  if (args.client_side_features_given) {
    // Client-side feature generation mode
    // Generate features and serialize into RankingRequest
    int batch_size = args.client_dlrm_batch_size_arg;
    int num_inferences = args.client_dlrm_inferences_arg;

    // Generate features
    auto dense_features = this_thread.feature_generator->generateDenseFeatures(batch_size);
    auto sparse_features = this_thread.feature_generator->generateSparseFeatures(batch_size);

    // Create RankingRequest with DLRMFeatures
    ranking::RankingRequest request;
    request.request_id() = static_cast<int64_t>(thread_id);
    request.num_inferences() = num_inferences;

    // Populate DLRMFeatures
    ranking::DLRMFeatures features;
    features.batch_size() = batch_size;
    features.num_dense_features() = args.client_num_dense_features_arg;
    features.num_sparse_features() = args.client_num_sparse_features_arg;

    // Convert float vector to double for Thrift
    ranking::DenseFeatureVector dense_vec;
    dense_vec.reserve(dense_features.size());
    for (float f : dense_features) {
      dense_vec.push_back(static_cast<double>(f));
    }
    features.dense_features() = std::move(dense_vec);
    features.sparse_features() = std::move(sparse_features);

    request.dlrm_features() = std::move(features);

    // Serialize the request
    folly::IOBufQueue bufq;
    apache::thrift::CompactSerializer::serialize(request, &bufq);
    auto buf = bufq.move();
    // Coalesce the IOBuf chain into a single contiguous buffer.
    // Without this, data() and length() only return the first segment,
    // causing truncated payloads and deserialization underflow errors.
    buf->coalesce();

    // Send request with serialized RankingRequest
    test_driver.sendRequest(ranking::kDLRMRequestType,
                            reinterpret_cast<const char*>(buf->data()),
                            buf->length(),
                            this_thread.request_delay);
  } else {
    // Original mode: send random string payload
    test_driver.sendRequest(ranking::kPageRankRequestType,
                            this_thread.random_string.c_str(), 3000,
                            this_thread.request_delay);
  }
}

int main(int argc, char **argv) {
  // Parse arguments
  if (cmdline_parser(argc, argv, &args) != 0) {
    std::cerr << "cmdline_parser failed" << std::endl;
    return 1;
  }

  // Check required arguments
  if (!args.server_given) {
    std::cerr << "--server must be specified." << std::endl;
    return 1;
  }

  auto host_port = ranking::utils::parseHostnameAndPort(args.server_arg);

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  feedsim::FeedSimDriver driver_node(host_port.first, host_port.second);

  driver_node.setThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));
  driver_node.setMakeRequestCallback(
      std::bind(MakeRequest, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));

  // Register only the request type that will be used
  // This ensures stats are collected for a single type, avoiding output parsing issues
  if (args.client_side_features_given) {
    driver_node.registerRequestType(ranking::kDLRMRequestType);
  } else {
    driver_node.registerRequestType(ranking::kPageRankRequestType);
  }

  // Enable remote monitoring
  driver_node.enableMonitoring(args.monitor_port_arg);

  // Log client-side feature generation mode
  if (args.client_side_features_given) {
    std::cout << "Client-side feature generation enabled:" << std::endl;
    std::cout << "  Batch size: " << args.client_dlrm_batch_size_arg << std::endl;
    std::cout << "  Inferences per request: " << args.client_dlrm_inferences_arg << std::endl;
    std::cout << "  Dense features: " << args.client_num_dense_features_arg << std::endl;
    std::cout << "  Sparse features: " << args.client_num_sparse_features_arg << std::endl;
    std::cout << "  Seed: " << args.client_feature_seed_arg << std::endl;
  }

  driver_node.run(args.threads_arg, args.affinity_given, args.connections_arg,
                  args.depth_arg);

  return 0;
}
